import re
data = open(r'D:/OBC/OBC/components/u8g2/csrc/u8g2_fonts.c', encoding='utf-8', errors='replace').read().split('\n')
ESC = {'n': 10, 'r': 13, 't': 9, 'a': 7, 'b': 8, 'f': 12, 'v': 11, '\\': 92, '"': 34}


def decode_str(s):
    out = bytearray()
    i = 0
    n = len(s)
    while i < n:
        if s[i] == '\\':
            j = i + 1
            if j < n and s[j] in '01234567':
                k = j
                while k < n and (k - j) < 3 and s[k] in '01234567':
                    k += 1
                out.append(int(s[j:k], 8))
                i = k
            else:
                out.append(ESC.get(s[j], ord(s[j])))
                i = j + 1
        else:
            out.append(ord(s[i]))
            i += 1
    return out


def get_font(name):
    start = None
    for i, l in enumerate(data):
        if ('const uint8_t ' + name + '[') in l and 'U8G2_FONT_SECTION' in l:
            start = i
            decl = l
            break
    if start is None:
        return None, 0
    m = re.search(r'\[(\d+)\]', decl)
    declared = int(m.group(1))
    out = bytearray()
    i = start + 1
    while i < len(data):
        l = data[i].strip()
        if not l.startswith('"'):
            break
        if l.endswith('";'):
            out += decode_str(l[1:-2])
            break
        out += decode_str(l[1:-1])
        i += 1
    return out, declared


b, declared = get_font('u8g2_font_wqy12_t_gb2312')
print('size:', len(b), 'declared:', declared)
print('glyph_cnt(byte0):', b[0])
print('start_pos_unicode(word@21):', b[21] | (b[22] << 8))
print('ascent@13:', b[13], 'descent@14:', b[14])
off = b[21] | (b[22] << 8)
if off < len(b):
    p = off
    for i in range(5):
        e = b[p] | (b[p + 1] << 8)
        sz = b[p + 2]
        print(f'  rec[{i}]: encoding=0x{e:04X} size={sz}')
        if e == 0 or sz == 0:
            break
        p += 3 + sz
