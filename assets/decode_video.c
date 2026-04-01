/*
 * 这是一个使用 libavcodec 的视频解码示例程序
 * 功能：从 MPEG1 视频文件中读取数据，解码为帧，并保存为 PGM 图片
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#define INBUF_SIZE 4096  // 输入缓冲区大小

// 将解码后的灰度帧保存为 PGM 文件
static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize,
                     char *filename)
{
    FILE *f;
    int i;

    f = fopen(filename,"wb"); // 打开文件
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255); // 写入PGM头信息
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f); // 写入每一行像素数据
    fclose(f); // 关闭文件
}

// 解码单个 AVPacket，并保存解码后的帧
static void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,
                   const char *filename)
{
    char buf[1024];
    int ret;

    // 将压缩包发送给解码器
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }

    // 循环接收解码后的帧
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) // 没有更多帧可解码
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        // 输出当前帧号
        printf("saving frame %3"PRId64"\n", dec_ctx->frame_num);
        fflush(stdout);

        // 构造输出文件名
        snprintf(buf, sizeof(buf), "%s-%"PRId64, filename, dec_ctx->frame_num);

        // 保存当前帧为 PGM 文件
        pgm_save(frame->data[0], frame->linesize[0],
                 frame->width, frame->height, buf);
    }
}

int main(int argc, char **argv)
{
    const char *filename, *outfilename;
    const AVCodec *codec;
    AVCodecParserContext *parser;
    AVCodecContext *c= NULL;
    FILE *f;
    AVFrame *frame;
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE]; // 输入缓冲区，多加 padding 防止越界
    uint8_t *data;
    size_t   data_size;
    int ret;
    int eof;
    AVPacket *pkt;

    // 检查命令行参数
    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n"
                "And check your input file is encoded by mpeg1video please.\n", argv[0]);
        exit(0);
    }
    filename    = argv[1];
    outfilename = argv[2];

    // 分配 AVPacket
    pkt = av_packet_alloc();
    if (!pkt)
        exit(1);

    // 填充输入缓冲区尾部，防止 MPEG 流越界读取
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    // 查找 MPEG1 视频解码器
    codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    // 初始化解析器
    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "parser not found\n");
        exit(1);
    }

    // 分配解码器上下文
    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    // 打开解码器
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    // 打开输入文件
    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }

    // 分配 AVFrame 用于存放解码后的帧
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    do {
        // 从输入文件读取原始数据
        data_size = fread(inbuf, 1, INBUF_SIZE, f);
        if (ferror(f))
            break;
        eof = !data_size; // 到达文件末尾

        // 使用解析器将数据拆分为单个帧
        data = inbuf;
        while (data_size > 0 || eof) {
            ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                fprintf(stderr, "Error while parsing\n");
                exit(1);
            }
            data      += ret;
            data_size -= ret;

            // 如果解析出了数据包，就解码
            if (pkt->size)
                decode(c, frame, pkt, outfilename);
            else if (eof)
                break;
        }
    } while (!eof);

    // 刷新解码器，处理剩余帧
    decode(c, frame, NULL, outfilename);

    // 关闭文件
    fclose(f);

    // 释放资源
    av_parser_close(parser);
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    return 0;
}