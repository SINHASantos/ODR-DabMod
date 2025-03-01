#!/usr/bin/env python
#
# This is an example program that shows
# how to receive runtime events from ODR-DabMod
#
# LICENSE: see bottom of file

import sys
import zmq
import json
from pprint import pprint

context = zmq.Context()
sock = context.socket(zmq.SUB)

ep = "tcp://127.0.0.1:5556"
print(f"Receive from {ep}")
sock.connect(ep)

# subscribe to all events
sock.setsockopt(zmq.SUBSCRIBE, bytes([]))

while True:
    parts = sock.recv_multipart()
    if len(parts) == 2:
        print("Received event '{}'".format(parts[0].decode()))
        pprint(json.loads(parts[1].decode()))

    else:
        print("Received strange event:")
        pprint(parts)

    print()


# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# For more information, please refer to <http://unlicense.org>
