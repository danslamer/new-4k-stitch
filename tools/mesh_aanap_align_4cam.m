% mesh_aanap_align_4cam.m
% ============================================================================
% 完整 AANAP (mesh-based) 把 4 张相机图对齐成 panorama
% ----------------------------------------------------------------------------
% 不用任何 toolbox (base MATLAB only).
%
% 流程:
%   1. 读 4 路 cam*.bin (column-major RGB uint8, 由 Python aanap_extract_features_mp4.py 生成)
%   2. 读 4 对 (i,j) 的 H_ij + inliers, 算 T_sim (similarity with regularization)
%   3. 链乘到 anchor (cam0):
%        T_anchor{1} = I
%        T_anchor{2} = H_01 * T_01
%        T_anchor{3} = H_02 * T_02
%        T_anchor{4} = H_01 * T_01 * H_13 * T_13
%   4. 对 cam1/2/3 每个做 mesh-based AANAP warp:
%        - 切 MESH_M x MESH_N cell (源 cam 像素空间)
%        - 每个 cell 内 inliers 估 local H_cell (DLT 2D affine)
%        - Adjust: H_cell_adj = (1-ALPHA_LOCAL)*H_cell + ALPHA_LOCAL*T_anchor{k}
%                  (缺 inlier 的 cell fallback 用 T_anchor{k})
%   5. Piecewise affine backward warp: 每个 cell 用 H_cell_adj + shift 投到 canvas
%   6. 4 张 warped 加权混合成 panorama
%   7. 落盘: 4 源图 montage / warped (含 mesh grid) / panorama / 重叠区放大
%
% 使用方法 (board 端或 PC 端):
%   >> cd <project_root>
%   >> mesh_aanap_align_4cam
%   或:
%   >> run('tools/mesh_aanap_align_4cam.m')
%
% 调参 (改 LAMBDA_REG / MESH_M / MESH_N / ALPHA_LOCAL):
%   - ALPHA_LOCAL: 1=纯 local H_cell, 0=纯 global T_anchor; 默认 0.5
%   - MESH_M / MESH_N: cell 数; 默认 16x16 (= 256 cells), 室内多平面场景
%     想要更细的 mesh 试 32x32 (= 1024 cells)
%   - LAMBDA_REG: T_sim 正则化系数; 默认 0.3, 0.1-0.5 都试过
%   - MIN_INL_PER_CELL: 低于此数 inlier 的 cell fallback 到 T_anchor; 默认 6
%
% 输入文件 (由 tools/aanap_extract_features_mp4.py 生成):
%   results/aanap_features_new/cam{0,1,2,3}.bin       1280x720x3 uint8 RGB
%   results/aanap_features_new/H_{i}_{j}.txt          3x3 homography
%   results/aanap_features_new/inl_{i}_{j}.txt        Nx4 [xi yi xj yj]
%
% 输出 (results/aanap_mesh_align/):
%   01_source_4cams.png          4 路源图
%   02_warped_3cams_with_mesh.png cam1/2/3 warped + mesh 边界 (红色)
%   03_panorama_final.png        4 张合成的 panorama
%   04_overlap_zoom.png          4 对 pair 重叠区灰度差热图
%   result.txt                   关键参数 + canvas 尺寸 + T_anchor
% ============================================================================

clear; close all;

%% ========== 配置 ==========
LAMBDA_REG     = 0.3;     % T_sim 正则化系数 (a->1, b->0, tx->0, ty->0)
MESH_M         = 16;      % mesh 横向 cell 数
MESH_N         = 16;      % mesh 纵向 cell 数
ALPHA_LOCAL    = 0.5;     % 1.0=纯 local H_cell, 0.0=纯 global T_anchor
                           % 0.5 推荐 (paper); 多平面 scene 可试 0.6-0.7
MIN_INL_PER_CELL = 6;     % 少于此数 inlier 的 cell fallback 用 T_anchor
WORK_W         = 1280;
WORK_H         = 720;
NUM_CAM        = 4;
MAX_CANVAS_W   = 4096;
MAX_CANVAS_H   = 2048;

% 4 路 + 4 对 (跟 Python 端 PAIRS 一致; 2x2 layout: (0,1)(2,3) 横向, (0,2)(1,3) 纵向)
PAIRS = [0 1; 0 2; 1 3; 2 3];

%% ========== 定位项目根 + 输出目录 ==========
PROJECT_ROOT = find_project_root();
FEAT_DIR     = fullfile(PROJECT_ROOT, 'results', 'aanap_features_new');
OUTPUT_DIR   = fullfile(PROJECT_ROOT, 'results', 'aanap_mesh_align');
if ~exist(OUTPUT_DIR, 'dir'); mkdir(OUTPUT_DIR); end
fprintf('FEAT_DIR   = %s\n', FEAT_DIR);
fprintf('OUTPUT_DIR = %s\n', OUTPUT_DIR);
fprintf('Config: mesh=%dx%d, alpha=%.2f, lambda=%.2f, min_inl/cell=%d\n', ...
    MESH_M, MESH_N, ALPHA_LOCAL, LAMBDA_REG, MIN_INL_PER_CELL);

%% ========== 读 4 路源图 ==========
imgs = cell(NUM_CAM, 1);
for i = 1:NUM_CAM
    bin_path = fullfile(FEAT_DIR, sprintf('cam%d.bin', i-1));
    if ~exist(bin_path, 'file')
        error('找不到 %s. 先跑 tools/aanap_extract_features_mp4.py.', bin_path);
    end
    fid = fopen(bin_path, 'rb');
    raw = fread(fid, WORK_W*WORK_H*3, 'uint8=>uint8');
    fclose(fid);
    imgs{i} = reshape(raw, [WORK_H WORK_W 3]);
    fprintf('  cam%d.bin: %dx%dx%d\n', i-1, WORK_H, WORK_W, 3);
end

%% ========== 读 4 对 H + inliers, 算 T_sim ==========
H_pair = cell(NUM_CAM, NUM_CAM);
T_pair = cell(NUM_CAM, NUM_CAM);
inl_pair = cell(NUM_CAM, NUM_CAM);

for p = 1:size(PAIRS, 1)
    i = PAIRS(p, 1);  j = PAIRS(p, 2);
    H_path  = fullfile(FEAT_DIR, sprintf('H_%d_%d.txt',   i, j));
    inl_path = fullfile(FEAT_DIR, sprintf('inl_%d_%d.txt', i, j));
    if ~exist(H_path, 'file') || ~exist(inl_path, 'file')
        warning('pair (%d,%d) 缺文件, 跳过', i, j);
        continue;
    end
    H_ij = readmatrix(H_path);
    inl  = readmatrix(inl_path);
    H_pair{i+1, j+1}  = H_ij;
    inl_pair{i+1, j+1} = inl;
    T_ij = compute_T_sim(H_ij, inl(:,1:2), inl(:,3:4), LAMBDA_REG);
    T_pair{i+1, j+1} = T_ij;
    fprintf('  pair (%d,%d): inliers=%d  T_sim=[a=%.3f b=%.3f tx=%.2f ty=%.2f]\n', ...
        i, j, size(inl,1), T_ij(1,1), T_ij(1,2), T_ij(1,3), T_ij(2,3));
end

% 反向
for i = 1:NUM_CAM
    for j = 1:NUM_CAM
        if isempty(H_pair{i,j}) && ~isempty(H_pair{j,i})
            H_pair{i,j}  = inv(H_pair{j,i});
            T_pair{i,j}  = inv(T_pair{j,i});
            inl_pair{i,j} = inl_pair{j,i}(:, [3 4 1 2]);
        end
    end
end

%% ========== 链乘到 anchor (cam0) ==========
% T_anchor{k} 把 cam k 像素投到 cam0 坐标 (3x3)
T_anchor = cell(NUM_CAM, 1);
T_anchor{1} = eye(3);
T_anchor{2} = H_pair{1,2} * T_pair{1,2};
T_anchor{3} = H_pair{1,3} * T_pair{1,3};
T_anchor{4} = H_pair{1,2} * T_pair{1,2} * H_pair{2,4} * T_pair{2,4};
for k = 1:NUM_CAM
    fprintf('  T_anchor{%d} = [\n', k);
    for r = 1:3
        fprintf('    %8.4f %8.4f %10.2f\n', T_anchor{k}(r,1), T_anchor{k}(r,2), T_anchor{k}(r,3));
    end
    fprintf('  ]\n');
end

%% ========== Canvas size (含 canvas cap 防 OOM) ==========
shift = eye(3);
shift(1,3) = WORK_W/2;
shift(2,3) = WORK_H/2;
corners = [0 0; WORK_W 0; WORK_W WORK_H; 0 WORK_H; 0 0];
corners_h = [corners ones(5,1)]';
canvas_w = 0; canvas_h = 0;
for i = 1:NUM_CAM
    M = shift * T_anchor{i};
    wc = M * corners_h;
    canvas_w = max(canvas_w, ceil(max(wc(1,:)./wc(3,:))));
    canvas_h = max(canvas_h, ceil(max(wc(2,:)./wc(3,:))));
end
canvas_w = canvas_w + 20;
canvas_h = canvas_h + 20;
if canvas_w > MAX_CANVAS_W
    s = MAX_CANVAS_W / canvas_w;
    canvas_w = MAX_CANVAS_W;
    canvas_h = ceil(canvas_h * s);
    shift(1,3) = shift(1,3) * s;  shift(2,3) = shift(2,3) * s;
    fprintf('  canvas 宽 cap: scale=%.3f\n', s);
end
if canvas_h > MAX_CANVAS_H
    s = MAX_CANVAS_H / canvas_h;
    canvas_h = MAX_CANVAS_H;
    canvas_w = ceil(canvas_w * s);
    shift(1,3) = shift(1,3) * s;  shift(2,3) = shift(2,3) * s;
    fprintf('  canvas 高 cap: scale=%.3f\n', s);
end
fprintf('\n=== Canvas = %dx%d ===\n', canvas_w, canvas_h);

%% ========== 为 cam1/2/3 算 mesh-based piecewise affine ==========
% 对 cam k (k=2,3,4): 把 cam k 像素空间切 MESH_M x MESH_N cell
%   每个 cell 用 inl_src{k} (在 cam k 空间) + inl_dst{k} (在 cam0 空间) 估 local H
%   H_cell_adj = (1-ALPHA_LOCAL) * H_cell + ALPHA_LOCAL * T_anchor{k}
%   缺 inlier 的 cell fallback 用 T_anchor{k}
%
% 收集每路 cam 的 inlier (源端 in cam k, 目标端 in cam0):
%   cam1: inl_pair{1,2}  [x0 y0 x1 y1] -> src=inl(:,3:4), dst=inl(:,1:2)
%   cam2: inl_pair{1,3}  [x0 y0 x2 y2] -> src=inl(:,3:4), dst=inl(:,1:2)
%   cam3: inl_pair{2,4}  [x1 y1 x3 y3] -> src=inl(:,3:4), dst=H_01 * inl(:,1:2)

inl_src = cell(NUM_CAM, 1);
inl_dst = cell(NUM_CAM, 1);
inl_src{1} = zeros(0, 2);  inl_dst{1} = zeros(0, 2);  % anchor, no warp
inl_src{2} = inl_pair{1,2}(:, 3:4);
inl_dst{2} = inl_pair{1,2}(:, 1:2);
inl_src{3} = inl_pair{1,3}(:, 3:4);
inl_dst{3} = inl_pair{1,3}(:, 1:2);
% cam3: inl_pair{2,4} 是 [x1,y1,x3,y3]; 把 (x1,y1) 经 H_01 投到 cam0
inl_31 = inl_pair{2,4}(:, 1:2);
inl_33 = inl_pair{2,4}(:, 3:4);
N = size(inl_31, 1);
H01 = H_pair{1,2};
p_h = [inl_31 ones(N,1)]';
p_w = H01 * p_h;
inl_dst_30 = [p_w(1,:)./p_w(3,:); p_w(2,:)./p_w(3,:)]';
inl_src{4} = inl_33;
inl_dst{4} = inl_dst_30;

% 估 cell H
H_cells = cell(NUM_CAM, 1);    % H_cells{k}{cy,cx} = 3x3 affine
cell_w_in = WORK_W / MESH_M;
cell_h_in = WORK_H / MESH_N;

for k = 2:NUM_CAM
    fprintf('\n--- Mesh AANAP for cam%d -> anchor ---\n', k-1);
    src = inl_src{k};
    dst = inl_dst{k};
    fprintf('  inliers: %d (cam%d -> cam0)\n', size(src,1), k-1);

    cell_H = cell(MESH_N, MESH_M);
    n_cells_filled = 0;
    n_cells_empty  = 0;
    for cy = 1:MESH_N
        for cx = 1:MESH_M
            x_lo = (cx-1) * cell_w_in;
            x_hi = cx * cell_w_in;
            y_lo = (cy-1) * cell_h_in;
            y_hi = cy * cell_h_in;
            in_cell = (src(:,1) >= x_lo) & (src(:,1) < x_hi) & ...
                      (src(:,2) >= y_lo) & (src(:,2) < y_hi);
            n_in_cell = sum(in_cell);

            if n_in_cell >= MIN_INL_PER_CELL
                src_c = src(in_cell, :);
                dst_c = dst(in_cell, :);
                % DLT 2D affine: dst = A * src + t
                X = [src_c ones(size(src_c,1), 1)];
                params_x = X \ dst_c(:,1);
                params_y = X \ dst_c(:,2);
                H_loc = eye(3);
                H_loc(1,:) = [params_x(1:2)' params_x(3)];
                H_loc(2,:) = [params_y(1:2)' params_y(3)];
                H_loc(3,:) = [0 0 1];

                % AANAP blend: 拉向 global T_anchor
                H_blend = (1-ALPHA_LOCAL) * H_loc + ALPHA_LOCAL * T_anchor{k};
                H_blend(3,:) = [0 0 1];
                cell_H{cy, cx} = H_blend;
                n_cells_filled = n_cells_filled + 1;
            else
                cell_H{cy, cx} = T_anchor{k};
                n_cells_empty = n_cells_empty + 1;
            end
        end
    end
    H_cells{k} = cell_H;
    fprintf('  cells: %d filled (DLT), %d fallback to T_anchor\n', ...
        n_cells_filled, n_cells_empty);
end

%% ========== Piecewise affine backward warp per cam ==========
fprintf('\n=== Warping 4 cams to canvas ===\n');
warped = cell(NUM_CAM, 1);
masks  = false(canvas_h, canvas_w, NUM_CAM);

% cam0: just shift to canvas
[warped{1}, masks(:,:,1)] = my_imwarp(imgs{1}, shift, canvas_h, canvas_w);
fprintf('  cam0 warped (anchor, no AANAP)\n');

for k = 2:NUM_CAM
    [warped{k}, masks(:,:,k)] = mesh_imwarp(imgs{k}, H_cells{k}, shift, ...
        canvas_h, canvas_w, MESH_M, MESH_N);
    fprintf('  cam%d warped (mesh AANAP)\n', k-1);
end

% Blend
pano = blend_images(warped, masks);

%% ========== Visualization ==========
fprintf('\n=== 落盘可视化 ===\n');

% (1) 4 路源图 montage
fig1 = figure('Name', '源图 4 路', 'Position', [100 100 1600 450]);
for k = 1:NUM_CAM
    subplot(1, 4, k);
    my_imshow(imgs{k});
    title(sprintf('cam%d (source, %dx%d)', k-1, WORK_W, WORK_H), 'FontSize', 12);
end
saveas(fig1, fullfile(OUTPUT_DIR, '01_source_4cams.png'));
fprintf('  01_source_4cams.png\n');

% (2) warped + mesh grid overlay
fig2 = figure('Name', 'warped 3 cams (mesh AANAP)', 'Position', [100 100 1600 1100]);
% anchor
subplot(2, 2, 1);
my_imshow(warped{1});
title('cam0 (anchor, no warp)', 'FontSize', 12);
for k = 2:NUM_CAM
    subplot(2, 2, k);
    overlay = draw_mesh_on_warped(warped{k}, H_cells{k}, shift, ...
        canvas_h, canvas_w, MESH_M, MESH_N, imgs{k}, T_anchor{k});
    my_imshow(overlay);
    title(sprintf('cam%d warped (mesh %dx%d, \\alpha=%.2f)', k-1, MESH_M, MESH_N, ALPHA_LOCAL), ...
        'FontSize', 12);
end
saveas(fig2, fullfile(OUTPUT_DIR, '02_warped_3cams_with_mesh.png'));
fprintf('  02_warped_3cams_with_mesh.png\n');

% (3) Final panorama
fig3 = figure('Name', 'panorama', 'Position', [100 100 1700 600]);
my_imshow(pano);
title(sprintf('AANAP mesh-based panorama (canvas %dx%d)', canvas_w, canvas_h), 'FontSize', 14);
saveas(fig3, fullfile(OUTPUT_DIR, '03_panorama_final.png'));
fprintf('  03_panorama_final.png\n');

% (4) Overlap 区域放大 (4 对 pair 的接缝处)
fig4 = figure('Name', 'overlap zoom', 'Position', [100 100 1400 1000]);
show_overlap_zoom(warped, masks, pano, NUM_CAM);
saveas(fig4, fullfile(OUTPUT_DIR, '04_overlap_zoom.png'));
fprintf('  04_overlap_zoom.png\n');

% (5) result.txt
fid = fopen(fullfile(OUTPUT_DIR, 'result.txt'), 'w');
fprintf(fid, 'AANAP Mesh-based 4-Camera Alignment\n');
fprintf(fid, '====================================\n');
fprintf(fid, 'Date      : %s\n', datestr(now));
fprintf(fid, 'FEAT_DIR  : %s\n', FEAT_DIR);
fprintf(fid, 'Canvas    : %dx%d\n', canvas_w, canvas_h);
fprintf(fid, 'Mesh      : %d x %d = %d cells\n', MESH_M, MESH_N, MESH_M*MESH_N);
fprintf(fid, 'Alpha local-global: %.2f (1=纯 local, 0=纯 global T_anchor)\n', ALPHA_LOCAL);
fprintf(fid, 'Lambda T_sim      : %.2f\n', LAMBDA_REG);
fprintf(fid, 'Min inlier/cell   : %d (少于 fallback 到 T_anchor)\n\n', MIN_INL_PER_CELL);
fprintf(fid, 'T_anchor per cam (3x3):\n');
for k = 1:NUM_CAM
    fprintf(fid, '  cam%d:\n', k-1);
    for r = 1:3
        fprintf(fid, '    %8.4f %8.4f %10.2f\n', T_anchor{k}(r,1), T_anchor{k}(r,2), T_anchor{k}(r,3));
    end
end
fclose(fid);
fprintf('  result.txt\n');

fprintf('\n=== 全部完成 ===\n');
fprintf('输出目录: %s\n', OUTPUT_DIR);

%% ====================== Local Functions ======================
function project_root = find_project_root()
%FIND_PROJECT_ROOT 找含 CMakeLists.txt 的目录.
    project_root = '';
    try
        f = matlab.desktop.editor.getActiveFilename();
        if ~isempty(f) && exist(f, 'file') == 2
            d = fileparts(f);
            [parent, ~] = fileparts(d);
            if exist(fullfile(parent, 'CMakeLists.txt'), 'file') == 2
                project_root = parent;  return;
            end
            cur = d;
            for kk = 1:6
                if exist(fullfile(cur, 'CMakeLists.txt'), 'file') == 2
                    project_root = cur;  return;
                end
                up = fileparts(cur);
                if isempty(up) || strcmp(up, cur); break; end
                cur = up;
            end
        end
    catch
    end
    cur = pwd;
    for kk = 1:6
        if exist(fullfile(cur, 'CMakeLists.txt'), 'file') == 2
            project_root = cur;  return;
        end
        up = fileparts(cur);
        if isempty(up) || strcmp(up, cur); break; end
        cur = up;
    end
    project_root = pwd;
end

function T_sim = compute_T_sim(H_ij, inl1, inl2, lambda)
% T_sim: similarity transform such that H * T_sim * p_j = p_i (with reg).
%   H_ij: H maps p_j -> p_i  (inlier1 in image i, inlier2 in image j)
    H_inv = inv(H_ij);
    N = size(inl1, 1);
    p1x = (H_inv(1,1)*inl1(:,1) + H_inv(1,2)*inl1(:,2) + H_inv(1,3)) ./ ...
          (H_inv(3,1)*inl1(:,1) + H_inv(3,2)*inl1(:,2) + H_inv(3,3));
    p1y = (H_inv(2,1)*inl1(:,1) + H_inv(2,2)*inl1(:,2) + H_inv(2,3)) ./ ...
          (H_inv(3,1)*inl1(:,1) + H_inv(3,2)*inl1(:,2) + H_inv(3,3));
    A = [inl2(:,1) -inl2(:,2) ones(N,1) zeros(N,1);
         inl2(:,2)  inl2(:,1) zeros(N,1) ones(N,1)];
    b_vec = [p1x; p1y];
    A_reg = [A; lambda * eye(4)];
    b_reg = [b_vec; lambda * [1; 0; 0; 0]];
    params = A_reg \ b_reg;
    a = params(1); b = params(2); tx = params(3); ty = params(4);
    T_sim = [a -b tx; b a ty; 0 0 1];
end

function [out, mask] = my_imwarp(img, M, out_h, out_w)
%MY_IMWARP 单 3x3 M 整图 backward + bilinear.
    [in_h, in_w, C] = size(img);
    out = zeros(out_h, out_w, C, 'uint8');
    M_inv = inv(M);
    [xx, yy] = meshgrid(1:out_w, 1:out_h);
    xs = xx(:)';  ys = yy(:)';  os = ones(1, out_w * out_h);
    x_src = (M_inv(1,1)*xs + M_inv(1,2)*ys + M_inv(1,3)*os) ./ ...
            (M_inv(3,1)*xs + M_inv(3,2)*ys + M_inv(3,3)*os);
    y_src = (M_inv(2,1)*xs + M_inv(2,2)*ys + M_inv(2,3)*os) ./ ...
            (M_inv(3,1)*xs + M_inv(3,2)*ys + M_inv(3,3)*os);
    valid = (x_src >= 1) & (x_src <= in_w) & (y_src >= 1) & (y_src <= in_h);
    mask = reshape(valid, out_h, out_w);
    x0 = floor(x_src);  y0 = floor(y_src);
    x1 = x0 + 1;        y1 = y0 + 1;
    wx = x_src - x0;    wy = y_src - y0;
    x0c = max(1, min(in_w, x0));  x1c = max(1, min(in_w, x1));
    y0c = max(1, min(in_h, y0));  y1c = max(1, min(in_h, y1));
    for c = 1:C
        I = double(img(:,:,c));
        Ia = I(sub2ind([in_h in_w], y0c, x0c));
        Ib = I(sub2ind([in_h in_w], y0c, x1c));
        Ic = I(sub2ind([in_h in_w], y1c, x0c));
        Id = I(sub2ind([in_h in_w], y1c, x1c));
        wxv = wx(:)';  wyv = wy(:)';
        val = (1-wxv).*(1-wyv).*Ia + wxv.*(1-wyv).*Ib + ...
              (1-wxv).*wyv.*Ic     + wxv.*wyv.*Id;
        ch = reshape(val, out_h, out_w);
        ch(~mask) = 0;
        out(:,:,c) = uint8(ch);
    end
end

function [out, mask] = mesh_imwarp(img, cell_H, shift, out_h, out_w, mesh_m, mesh_n)
% MESH_IMWARP piecewise affine: 每个 cell 用 cell_H{cy,cx} 投到 canvas.
    [in_h, in_w, C] = size(img);
    out  = zeros(out_h, out_w, C, 'uint8');
    mask = false(out_h, out_w);

    cell_w_in = in_w / mesh_m;
    cell_h_in = in_h / mesh_n;

    % Step 1: 算每个 cell 在 canvas 上的 bbox (前向 warp cell 4 角)
    cell_ext = zeros(mesh_n, mesh_m, 4);   % [xmin ymin xmax ymax]
    for cy = 1:mesh_n
        for cx = 1:mesh_m
            H_full = shift * cell_H{cy, cx};
            x_lo = (cx-1) * cell_w_in;
            y_lo = (cy-1) * cell_h_in;
            x_hi = cx * cell_w_in;
            y_hi = cy * cell_h_in;
            corners = [x_lo y_lo; x_hi y_lo; x_hi y_hi; x_lo y_hi];
            N = 4;
            p_h = [corners ones(N,1)]';
            p_w = H_full * p_h;
            xs = p_w(1,:) ./ p_w(3,:);
            ys = p_w(2,:) ./ p_w(3,:);
            cell_ext(cy,cx,:) = [min(xs) min(ys) max(xs) max(ys)];
        end
    end

    % Step 2: backward warp 每个 cell 的 bbox 区域
    for cy = 1:mesh_n
        for cx = 1:mesh_m
            x_min = max(1, ceil(cell_ext(cy,cx,1)));
            y_min = max(1, ceil(cell_ext(cy,cx,2)));
            x_max = min(out_w, floor(cell_ext(cy,cx,3)));
            y_max = min(out_h, floor(cell_ext(cy,cx,4)));
            if x_max < x_min || y_max < y_min
                continue;
            end
            H_full_inv = inv(shift * cell_H{cy, cx});
            [xx, yy] = meshgrid(x_min:x_max, y_min:y_max);
            Np = numel(xx);
            xs = xx(:)';  ys = yy(:)';  os = ones(1, Np);
            x_src = (H_full_inv(1,1)*xs + H_full_inv(1,2)*ys + H_full_inv(1,3)*os) ./ ...
                    (H_full_inv(3,1)*xs + H_full_inv(3,2)*ys + H_full_inv(3,3)*os);
            y_src = (H_full_inv(2,1)*xs + H_full_inv(2,2)*ys + H_full_inv(2,3)*os) ./ ...
                    (H_full_inv(3,1)*xs + H_full_inv(3,2)*ys + H_full_inv(3,3)*os);

            valid = (x_src >= 1) & (x_src <= in_w) & (y_src >= 1) & (y_src <= in_h);
            if ~any(valid)
                continue;
            end
            mask(y_min:y_max, x_min:x_max) = mask(y_min:y_max, x_min:x_max) | ...
                reshape(valid, size(xx));

            x0 = floor(x_src);  y0 = floor(y_src);
            x1 = x0 + 1;        y1 = y0 + 1;
            wx = x_src - x0;    wy = y_src - y0;
            x0c = max(1, min(in_w, x0));  x1c = max(1, min(in_w, x1));
            y0c = max(1, min(in_h, y0));  y1c = max(1, min(in_h, y1));

            for c = 1:C
                I = double(img(:,:,c));
                Ia = I(sub2ind([in_h in_w], y0c, x0c));
                Ib = I(sub2ind([in_h in_w], y0c, x1c));
                Ic = I(sub2ind([in_h in_w], y1c, x0c));
                Id = I(sub2ind([in_h in_w], y1c, x1c));
                wxv = wx(:)';  wyv = wy(:)';
                val = (1-wxv).*(1-wyv).*Ia + wxv.*(1-wyv).*Ib + ...
                      (1-wxv).*wyv.*Ic     + wxv.*wyv.*Id;
                ch = reshape(val, size(xx));
                ch(~reshape(valid, size(xx))) = 0;
                out(y_min:y_max, x_min:x_max, c) = uint8(ch);
            end
        end
    end
end

function pano = blend_images(images, masks)
%BLEND_IMAGES 4 张 warped + mask -> 平均混合 uint8 panorama.
    [canvas_h, canvas_w] = size(masks, [1 2]);
    pano = zeros(canvas_h, canvas_w, 3, 'double');
    weight = zeros(canvas_h, canvas_w, 'double');
    for i = 1:size(images,1)
        if isempty(images{i}); continue; end
        m = double(masks(:,:,i));
        for c = 1:3
            pano(:,:,c) = pano(:,:,c) + double(images{i}(:,:,c)) .* m;
        end
        weight = weight + m;
    end
    weight(weight == 0) = 1;
    for c = 1:3
        pano(:,:,c) = pano(:,:,c) ./ weight;
    end
    pano = uint8(pano);
end

function overlay = draw_mesh_on_warped(warped_img, cell_H, shift, canvas_h, canvas_w, ...
    mesh_m, mesh_n, src_img, T_anchor_k)
% DRAW_MESH_ON_WARPED 把 cell 边界 forward warp 到 canvas, 用红色 (255,0,0) 标出 mesh.
%   看的出 mesh 网格的形变.
    overlay = warped_img;
    in_h = size(src_img, 1);
    in_w = size(src_img, 2);
    cell_w = in_w / mesh_m;
    cell_h = in_h / mesh_n;
    N_samples = 32;
    plane = canvas_h * canvas_w;
    plane2 = 2 * plane;

    % 水平线 (cy = 0..mesh_n)
    for cy = 0:mesh_n
        y_src = cy * cell_h;
        for cx = 1:mesh_m
            cy_pick = max(1, min(mesh_n, cy));
            H_pick = shift * cell_H{cy_pick, cx};
            xs = linspace((cx-1) * cell_w, cx * cell_w, N_samples);
            ys = y_src * ones(1, N_samples);
            p_h = [xs; ys; ones(1, N_samples)];
            p_w = H_pick * p_h;
            px = round(p_w(1,:) ./ p_w(3,:));
            py = round(p_w(2,:) ./ p_w(3,:));
            valid = (px >= 1) & (px <= canvas_w) & (py >= 1) & (py <= canvas_h);
            if ~any(valid); continue; end
            idx = sub2ind([canvas_h canvas_w], py(valid), px(valid));
            overlay(idx)         = 255;  % R
            overlay(idx + plane)  = 0;
            overlay(idx + plane2) = 0;
        end
    end
    % 垂直线 (cx = 0..mesh_m)
    for cx = 0:mesh_m
        x_src = cx * cell_w;
        for cy = 1:mesh_n
            cx_pick = max(1, min(mesh_m, cx));
            H_pick = shift * cell_H{cy, cx_pick};
            ys = linspace((cy-1) * cell_h, cy * cell_h, N_samples);
            xs = x_src * ones(1, N_samples);
            p_h = [xs; ys; ones(1, N_samples)];
            p_w = H_pick * p_h;
            px = round(p_w(1,:) ./ p_w(3,:));
            py = round(p_w(2,:) ./ p_w(3,:));
            valid = (px >= 1) & (px <= canvas_w) & (py >= 1) & (py <= canvas_h);
            if ~any(valid); continue; end
            idx = sub2ind([canvas_h canvas_w], py(valid), px(valid));
            overlay(idx)         = 255;
            overlay(idx + plane)  = 0;
            overlay(idx + plane2) = 0;
        end
    end
end

function show_overlap_zoom(warped, masks, pano, num_cam)
% SHOW_OVERLAP_ZOOM 4 对相邻 pair 的 overlap 区放大 + 灰度差热图.
    pairs_local = [1 2; 1 3; 2 4; 3 4];
    labels_local = {'(0,1) cam0-cam1', '(0,2) cam0-cam2', '(1,3) cam1-cam3', '(2,3) cam2-cam3'};
    for p = 1:4
        i = pairs_local(p, 1);
        j = pairs_local(p, 2);
        subplot(2, 2, p);
        overlap = masks(:,:,i) & masks(:,:,j);
        if ~any(overlap(:))
            title(sprintf('%s: no overlap', labels_local{p}));
            axis image; axis off; continue;
        end
        g1 = my_rgb2gray(warped{i});
        g2 = my_rgb2gray(warped{j});
        diff_ij = abs(double(g1) - double(g2));
        diff_masked = diff_ij .* double(overlap);
        imagesc(diff_masked); colormap(gca, 'hot'); colorbar; axis image;
        mean_d = mean(diff_ij(overlap));
        max_d  = max(diff_ij(overlap));
        title(sprintf('%s: overlap mean=%.1f max=%d', labels_local{p}, mean_d, max_d), 'FontSize', 10);
    end
end

function g = my_rgb2gray(rgb)
    if size(rgb, 3) == 1
        g = rgb; return;
    end
    r = double(rgb(:,:,1));  gch = double(rgb(:,:,2));  b = double(rgb(:,:,3));
    g = uint8(0.299 * r + 0.587 * gch + 0.114 * b);
end

function my_imshow(rgb)
    [h, w, c] = size(rgb);
    if c == 3
        rgb_d = double(rgb) / 255;
    elseif c == 1
        rgb_d = double(rgb);
        if max(rgb_d(:)) > 1
            rgb_d = rgb_d / 255;
        end
    else
        rgb_d = double(rgb);
    end
    image(rgb_d);
    set(gca, 'XTick', [], 'YTick', []);
    axis image;
end
