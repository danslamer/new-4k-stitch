#!/usr/bin/env python3
"""tools/_yaml_set_cams.py
Apply IP list + optional user/password/path to params/camera_sources.yaml.
Used by tools/set_cam_ips.sh (called as subprocess; avoids heredoc quoting hell).

Usage:
  python3 _yaml_set_cams.py YAML IP1 IP2 ... IPn [USER] [PASSWORD] [PATH]

Constraints:
  - Anchored to "    uri:" lines (re.MULTILINE) so yaml comments are not modified
  - Up to N (number of "    uri:" lines in yaml) hosts replaced
  - URI regex limited to single line (no multiline matches) via $ anchor
"""
import sys
import re

yaml_path = sys.argv[1]
# argv[2..] are IPs first, then optionally [USER] [PASSWORD] [PATH]
# Heuristic: if there are > 6 IPs, treat last 3 as user/password/path.
# Simpler: caller passes n_ips + n_yaml first. But for compat with shell:
#   argv[1] = yaml, argv[2..7] = 6 ips, argv[8] = user, argv[9] = pwd, argv[10] = path
# If fewer than 6 actual URIs, n_ips and n_yaml may differ; we handle up to len(ips).

ips = sys.argv[2:8] if len(sys.argv) >= 8 else sys.argv[2:]
new_user = sys.argv[8] if len(sys.argv) > 8 else ""
new_pwd = sys.argv[9] if len(sys.argv) > 9 else ""
new_path = sys.argv[10] if len(sys.argv) > 10 else ""

with open(yaml_path, "r", encoding="utf-8") as f:
    text = f.read()

# Step 1: replace host portion of each "    uri:" line. Anchored so
# comments mentioning rtsp:// are not touched.
uri_pat = re.compile(
    r"^(    uri:[^\n]*?rtsp://)([^@/]+@)([A-Za-z0-9.\-]+)(:[0-9]+(/\S*)?)$",
    re.MULTILINE,
)
counter = [0]
n_yaml = 0
def replace_host(match):
    global n_yaml
    if n_yaml == 0:
        # First invocation: count matches
        pass
    n_yaml += 1
    if counter[0] >= len(ips):
        return match.group(0)
    new_ip = ips[counter[0]]
    counter[0] += 1
    return match.group(1) + match.group(2) + new_ip + match.group(4)

text = uri_pat.sub(replace_host, text)
print(f"  step1 host: {counter[0]}/{n_yaml} replaced", file=sys.stderr)

# Step 2: replace URI embedded password (line anchored).
if new_pwd:
    pwd_pat = re.compile(
        r"^(    uri:[^\n]*?rtsp://[^@/]+:)([^@\s]+)(@)",
        re.MULTILINE,
    )
    text, n = pwd_pat.subn(
        lambda m: m.group(1) + new_pwd + m.group(3),
        text,
    )
    print(f"  step2 pwd-in-uri: {n} replaced", file=sys.stderr)

# Step 3: replace URI path (line anchored).
if new_path:
    if not new_path.startswith("/"):
        new_path = "/" + new_path
    path_pat = re.compile(
        r"^(    uri:[^\n]*?rtsp://[^/]+@[^:/]+:[0-9]+)(/\S+)?",
        re.MULTILINE,
    )
    text = path_pat.sub(lambda m: m.group(1) + new_path, text)
    print(f"  step3 path: replaced", file=sys.stderr)

# Step 4: replace user_id / user_pw fields (anchored to those field names).
if new_user:
    text = re.sub(
        r"^(\s*user_id:\s*).+$",
        lambda m: m.group(1) + new_user,
        text,
        flags=re.MULTILINE,
    )
    print(f"  step4 user_id: replaced", file=sys.stderr)
if new_pwd:
    text = re.sub(
        r"^(\s*user_pw:\s*).+$",
        lambda m: m.group(1) + new_pwd,
        text,
        flags=re.MULTILINE,
    )
    print(f"  step4 user_pw: replaced", file=sys.stderr)

with open(yaml_path, "w", encoding="utf-8") as f:
    f.write(text)

print(f"  OK", file=sys.stderr)