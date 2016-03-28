#!/usr/bin/env python

import sys

if len(sys.argv) < 2:
    print("usage: %s <outname> <prefix>:<file> ..." % sys.argv[0])
    sys.exit(0)

base = sys.argv[1]
header= '#ifndef %s_h\n' % base
header += '#define %s_h\n\n' % base
src = '#include "%s.h"\n\n' % base

prefixes = []
files = []

for i in range(2, len(sys.argv)):
    prefix, infile = sys.argv[i].split(':')

    header += 'extern char _%sbuf[];\n' % prefix
    header += 'extern int _%sbuf_len;\n\n' % prefix
    src += 'char _%sbuf[] = {' % prefix

    buf = open(infile, 'rb').read()
    for j, b in enumerate(buf):
        if j % 12 == 0:
            src += '\n  '

        # ugly but works without future pkg in python2
        if (type(b) == str):
            src += '0x%02x, ' % ord(b)
        else:
            src += '0x%02x, ' % b

    src += '\n};\n\n'
    src += 'int _%sbuf_len = sizeof(_%sbuf);\n\n' % (prefix, prefix)

header += '#endif'

with open('%s.h' % base, 'w') as f:
    print('PY %s.h' % base)
    f.write(header)

with open('%s.cc' % base, 'w') as f:
    print('PY %s.cc' % base)
    f.write(src)
