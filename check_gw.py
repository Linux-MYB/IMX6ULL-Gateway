# -*- coding: utf-8 -*-
"""gw 工程静态自检：括号配对、头文件覆盖、声明与定义对齐。"""
import os
import re
import sys

ROOT = r"C:\Users\61136\Documents\deepseek-harness\default-workspace\gw"
FILES = ["src/main.c", "src/mailbox.c", "src/modbus.c", "src/canbus.c",
         "src/sensor.c", "src/store.c", "src/alarm.c", "src/ui.c",
         "src/upload.c", "lib/framebuffer.c"]

# 用到某 API 时需要的头文件
NEED = {
    "malloc": "<stdlib.h>", "calloc": "<stdlib.h>", "free": "<stdlib.h>",
    "atoi": "<stdlib.h>", "atof": "<stdlib.h>", "exit": "<stdlib.h>",
    "strncpy": "<string.h>", "strcmp": "<string.h>", "strstr": "<string.h>",
    "memcpy": "<string.h>", "memset": "<string.h>", "strlen": "<string.h>",
    "strncmp": "<string.h>", "strerror": "<string.h>",
    "printf": "<stdio.h>", "snprintf": "<stdio.h>", "sprintf": "<stdio.h>",
    "fopen": "<stdio.h>", "fclose": "<stdio.h>", "fscanf": "<stdio.h>",
    "fgets": "<stdio.h>", "sscanf": "<stdio.h>", "perror": "<stdio.h>",
    "open": "<fcntl.h>", "close": "<unistd.h>", "read": "<unistd.h>",
    "write": "<unistd.h>", "usleep": "<unistd.h>", "sleep": "<unistd.h>",
    "unlink": "<unistd.h>",
    "time": "<time.h>", "localtime": "<time.h>",
    "clock_gettime": "<time.h>",
    "pthread_create": "<pthread.h>", "pthread_mutex_lock": "<pthread.h>",
    "pthread_cond_wait": "<pthread.h>", "pthread_join": "<pthread.h>",
    "signal": "<signal.h>",
    "memcmp": "<string.h>",
}


def strip_code(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
        elif c in '"\'':
            q = c
            out.append(' ')
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == q:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check_braces(name, code):
    pairs = {')': '(', ']': '[', '}': '{'}
    stack, line = [], 1
    for ch in code:
        if ch == '\n':
            line += 1
        elif ch in '([{':
            stack.append((ch, line))
        elif ch in ')]}':
            if not stack or stack[-1][0] != pairs[ch]:
                return "第 %d 行多余的 %s" % (line, ch)
            stack.pop()
    if stack:
        return "第 %d 行的 %s 没闭合" % (stack[-1][1], stack[-1][0])
    return None


problems = 0
for rel in FILES:
    path = os.path.join(ROOT, rel)
    raw = open(path, encoding="utf-8").read()
    code = strip_code(raw)

    err = check_braces(rel, code)
    if err:
        print("[括号] %s: %s" % (rel, err))
        problems += 1

    includes = set(re.findall(r'#include\s*[<"]([^>"]+)[>"]', raw))
    # 间接包含：gw.h 里带了 pthread.h / time.h
    includes |= {"pthread.h", "time.h"}
    names = set(os.path.basename(x) for x in includes)
    for api, hdr in NEED.items():
        if re.search(r'\b' + api + r'\s*\(', code):
            if os.path.basename(hdr.strip("<>")) not in names:
                print("[头文件] %s: 用了 %s() 但没包含 %s" % (rel, api, hdr))
                problems += 1

print("-\n检查完成，问题数:", problems)
sys.exit(1 if problems else 0)

