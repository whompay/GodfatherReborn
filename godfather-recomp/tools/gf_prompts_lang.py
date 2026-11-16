"""gf_prompts_lang.py -- PC versions of the SIXAXIS motion prompts for a language pack (Spanish).

The English table (gf_prompts_table.h, tools/gen_prompts.py) rewrites the BLES00043 prompts that tell the player to
move the SIXAXIS controller. A language disc's prompts are different strings, so the language pack carries its own:
this reads the disc's string tables and writes <pack>/gf_prompts.txt, one prompt per line, three hex fields
(original, controller version, keyboard/mouse version; each no longer than the original, as gf_prompts.c rewrites
the strings in place). Only Spanish phrasing is known so far; other languages produce no file.
library: build(lang_usrdir, table_paths, code, out_path) -> number of prompts"""
import re, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gf_str

C = r'(?:\[c=CUSTOM1\])?'; R = r'(?:\[c=RESET\])?'
def w(x): return C + x + R

ES_VERB = C + r'(MUEVE|mueve|mover|GIRA|gira|EMPUJA|empuja|TIRA|tira)' + R + r' (?:el|del) +mando inal\xe1mbrico SIXAXIS\xac '
IZQ, DER = r'(?:HACIA LA IZQUIERDA|hacia la izquierda)', r'(?:HACIA LA DERECHA|hacia la derecha)'
ES_DIRS = [
    ('UPTOWARD', w('(?:HACIA ARRIBA Y HACIA|hacia arriba y hacia)') + ' ti'),
    ('KNEE_L',   w('(?:HACIA ABAJO, HACIA DENTRO|hacia abajo, hacia dentro)') + ' y hacia la ' + w('(?:IZQUIERDA|izquierda)')),
    ('KNEE_R',   w('(?:HACIA ABAJO, HACIA DENTRO|hacia abajo, hacia dentro)') + ' y hacia la ' + w('(?:DERECHA|derecha)')),
    ('KNEE',     w('(?:HACIA ABAJO|hacia abajo)') + ' y ' + w('(?:HACIA DENTRO|hacia dentro)')),
    ('FWDBACK',  w('(?:HACIA DELANTE|hacia delante)') + ' y ' + w('(?:HACIA ATR\xc1S|hacia atr\xe1s)')),
    ('LR',       w(IZQ) + ' o ' + w(DER)),
    ('LR',       r'(?:a izquierda o derecha|a la izquierda o a la derecha)'),
    ('TWIST',    w('(?:EN EL SENTIDO DE LAS AGUJAS DEL RELOJ|en el sentido de las agujas del reloj)')),
    ('BACK',     w('(?:HACIA|hacia)') + ' ti'),
    ('FWD',      w('(?:HACIA DELANTE|hacia delante)')),
    ('L',        w(IZQ)), ('R', w(DER)),
    ('UP',       w('(?:HACIA ARRIBA|hacia arriba)')),
    ('DOWN',     w('(?:HACIA ABAJO|hacia abajo)')),
]
KBM = {'FWD': 'X+W', 'BACK': 'X+S', 'L': 'X+A', 'R': 'X+D', 'LR': 'X+A[c=RESET] o [c=CUSTOM1]X+D',
       'FWDBACK': 'X+W[c=RESET] y [c=CUSTOM1]X+S', 'UP': 'X+SPACE', 'UPTOWARD': 'X+SPACE', 'TILT': 'X+SPACE',
       'DOWN': 'X+CTRL', 'KNEE': 'X+CTRL', 'KNEE_L': 'X+CTRL+A', 'KNEE_R': 'X+CTRL+D', 'TWIST': 'X+E'}

def es_pad(g, up, inf):
    v = (lambda imp, infv: infv if inf else imp)
    cu = lambda s: '[c=CUSTOM1]' + (s if up else s.lower()) + '[c=RESET]'
    MOVE, PRESS, HOLD, TURN = v('MUEVE', 'MOVER'), v('PULSA', 'PULSAR'), v('MANT\xc9N', 'MANTENER'), v('GIRA', 'GIRAR')
    st = '[b=Rstick]'
    return {'FWD': cu(MOVE) + ' ' + st + ' ' + cu('HACIA ARRIBA'), 'BACK': cu(MOVE) + ' ' + st + ' ' + cu('HACIA ABAJO'),
            'L': cu(MOVE) + ' ' + st + ' ' + cu('HACIA LA IZQUIERDA'), 'R': cu(MOVE) + ' ' + st + ' ' + cu('HACIA LA DERECHA'),
            'LR': cu(MOVE) + ' ' + st + ' ' + cu('HACIA LA IZQUIERDA') + ' o ' + cu('HACIA LA DERECHA'),
            'FWDBACK': cu(MOVE) + ' ' + st + ' ' + cu('HACIA ARRIBA') + ' y ' + cu('HACIA ABAJO'),
            'UP': cu(PRESS) + ' [RightStickDepress]', 'UPTOWARD': cu(PRESS) + ' [RightStickDepress]', 'TILT': cu(PRESS) + ' [RightStickDepress]',
            'DOWN': cu(PRESS) + ' [LeftStickDepress]', 'KNEE': cu(PRESS) + ' [LeftStickDepress]',
            'KNEE_L': cu(HOLD) + ' [LeftStickDepress] + ' + cu(MOVE) + ' ' + st + ' ' + cu('HACIA LA IZQUIERDA'),
            'KNEE_R': cu(HOLD) + ' [LeftStickDepress] + ' + cu(MOVE) + ' ' + st + ' ' + cu('HACIA LA DERECHA'),
            'TWIST': cu(TURN) + ' [RightStickClockwise]'}[g]

def es_kbm(g, up, inf):
    p = ('PULSAR' if inf else 'PULSA')
    return '[c=CUSTOM1]' + (p if up else p.lower()) + '[c=RESET] [c=CUSTOM1]' + KBM[g] + '[c=RESET]'

def rewrite_es(t):
    """(controller, keyboard/mouse) versions of one prompt, or None."""
    if 'Vuelve a conectar' in t:
        r = t.replace('el mando inal\xe1mbrico SIXAXIS\xac', 'el mando')
        return r, r
    m = re.search(ES_VERB, t)
    if not m: return None
    rest = t[m.end():]; g = mm = None
    for name, rx in ES_DIRS:
        mm = re.match(rx, rest)
        if mm: g = name; break
    if not g: return None
    verb = m.group(1)
    if verb.upper() == 'GIRA' and g == 'UP': g = 'TILT'
    up = verb.isupper(); inf = verb == 'mover'
    a, b = m.start(), m.end() + mm.end()
    styled = t[a:a + 3] == '[c='
    pp, kk = es_pad(g, up, inf), es_kbm(g, up, inf)
    if not styled:
        pp = re.sub(r'\[c=[A-Z0-9_]+\]', '', pp); kk = re.sub(r'\[c=[A-Z0-9_]+\]', '', kk)
    p = t[:a] + pp + t[b:]
    k = (t[:a] + kk + t[b:]).replace('[LSTop]', 'RMB').replace('[RSTop]', 'LMB')
    return p, k

def table_strings(path):
    u = next(u for o, u in gf_str.chunks(path))
    n = struct.unpack('>I', u[0xA0:0xA4])[0]
    offs = struct.unpack('>%dI' % n, u[0xB0 + 4 * n:0xB0 + 8 * n])
    return [u[0x90 + o:u.index(b'\0', 0x90 + o)] for o in offs]

def build(table_paths, code, out_path):
    if code != 'es': return 0
    seen = set(); rows = []
    for tp in table_paths:
        for s in table_strings(tp):
            if b'SIXAXIS\xac' not in s or s in seen: continue
            seen.add(s)
            r = rewrite_es(s.decode('latin1'))
            if not r: continue
            p, k = (x.encode('latin1') for x in r)
            if len(p) > len(s) or len(k) > len(s): continue
            rows.append((s, p, k))
    with open(out_path, 'w') as f:
        f.write('# gf_prompts.txt (%s) -- tools/gf_prompts_lang.py: original<TAB>controller<TAB>keyboard/mouse, hex\n' % code)
        for s, p, k in rows: f.write('%s\t%s\t%s\n' % (s.hex(), p.hex(), k.hex()))
    return len(rows)
