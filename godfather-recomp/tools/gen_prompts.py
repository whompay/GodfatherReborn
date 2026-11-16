import pickle,re,sys
allu=pickle.load(open('allu.pkl','rb'))
en=sorted(s for s,f in allu.items() if any('310D' in x or '96686' in x for x in f))
C=r'(?:\[c=CUSTOM1\])?'; R=r'(?:\[c=RESET\])?'
def w(x): return C+x+R
DIRS=[ ('UPTOWARD', w('(?:UP TOWARDS)')+' your body'),
       ('BACK',     w('(?:TOWARDS)')+' your body'),
       ('KNEE_L',   w('DOWN, IN')+', and to the '+w('LEFT')),
       ('KNEE_R',   w('DOWN, IN')+', and to the '+w('RIGHT')),
       ('KNEE',     w('(?:DOWN|down)')+' and '+w('(?:IN|in)')+' to your sides'),
       ('FWDBACK',  w('FORWARD')+' and '+w('BACK')),
       ('LR',       w('(?:LEFT|left)')+' or '+w('(?:RIGHT|right)')),
       ('TWIST',    w('CLOCKWISE')),
       ('FWD',      w('(?:FORWARD|forward)')),
       ('L',        w('(?:LEFT|left)')),
       ('R',        w('(?:RIGHT|right)')),
       ('UP',       w('(?:UP|up)')),
       ('DOWN',     w('(?:DOWN|down)')) ]
VERB=C+r'(move|MOVE|pull|PULL|push|ROTATE|TWIST)'+R+r' the SIXAXIS\xac wireless controller '
KBM={'FWD':'G+W','BACK':'G+S','L':'G+A','R':'G+D','LR':'G+A[c=RESET] or [c=CUSTOM1]G+D','FWDBACK':'G+W[c=RESET] and [c=CUSTOM1]G+S',
     'UP':'G+SPACE','UPTOWARD':'G+SPACE','TILT':'G+SPACE','DOWN':'G+CTRL','KNEE':'G+CTRL','KNEE_L':'G+CTRL+A','KNEE_R':'G+CTRL+D','TWIST':'G+E'}
def pad(g,up):
    cu=lambda s: '[c=CUSTOM1]'+(s if up else s.lower())+'[c=RESET]'
    st='[b=Rstick]'
    return {'FWD':cu('FLICK')+' '+st+' '+cu('UP'),'BACK':cu('FLICK')+' '+st+' '+cu('DOWN'),
            'L':cu('FLICK')+' '+st+' '+cu('LEFT'),'R':cu('FLICK')+' '+st+' '+cu('RIGHT'),
            'LR':cu('FLICK')+' '+st+' '+cu('LEFT')+' or '+cu('RIGHT'),
            'FWDBACK':cu('FLICK')+' '+st+' '+cu('UP')+' and '+cu('DOWN'),
            'UP':cu('PRESS')+' [RightStickDepress]','UPTOWARD':cu('PRESS')+' [RightStickDepress]','TILT':cu('PRESS')+' [RightStickDepress]',
            'DOWN':cu('PRESS')+' [LeftStickDepress]','KNEE':cu('PRESS')+' [LeftStickDepress]',
            'KNEE_L':cu('HOLD')+' [LeftStickDepress] + '+cu('FLICK')+' '+st+' '+cu('LEFT'),
            'KNEE_R':cu('HOLD')+' [LeftStickDepress] + '+cu('FLICK')+' '+st+' '+cu('RIGHT'),
            'TWIST':cu('ROTATE')+' [RightStickClockwise]'}[g]
def slam_keys(s):
    """v1130H EA PC: near a low object the side slam is A / D (mouse left / right stays swing / throw)."""
    C, R = '[c=CUSTOM1]', '[c=RESET]'
    for v, a in (('DRAG', 'PRESS'), ('drag', 'press'), ('ARRASTRA', 'PULSA'), ('arrastra', 'pulsa'), ('ARRASTRAR', 'PULSAR'), ('arrastrar', 'pulsar')):
        for st in (True, False):
            V = (C + v + R) if st else v; A_ = (C + a + R) if st else a
            K = (lambda k: C + k + R) if st else (lambda k: k)
            for m, l, r_, o_ in ((' the mouse ', ('LEFT', 'left'), ('RIGHT', 'right'), ' or '),
                                 (' el rat\xf3n ', ('HACIA LA IZQUIERDA', 'hacia la izquierda'), ('HACIA LA DERECHA', 'hacia la derecha'), ' o ')):
                for L_, R2 in zip(l, r_):
                    s = s.replace(V + m + K(L_) + o_ + K(R2), A_ + ' ' + K('A') + o_ + K('D'))
                    s = s.replace(V + m + K(L_), A_ + ' ' + K('A')).replace(V + m + K(R2), A_ + ' ' + K('D'))
    return s
# v1130F EA-style gestures (no gesture key): keys / mouse while grabbing or locked on
C_ = '[c=CUSTOM1]'; R_ = '[c=RESET]'
def K_(s): return C_ + s + R_
def en_phrase(g, up):
    P=lambda v: C_+(v if up else v.lower())+R_
    return {'FWD':P('PRESS')+' '+K_('W'), 'BACK':P('PULL')+' the mouse '+K_('BACK'),
            'L':P('DRAG')+' the mouse '+K_('LEFT'), 'R':P('DRAG')+' the mouse '+K_('RIGHT'),
            'LR':P('DRAG')+' the mouse '+K_('LEFT')+' or '+K_('RIGHT'),
            'FWDBACK':P('PRESS')+' '+K_('W')+' and '+P('PULL')+' the mouse '+K_('BACK'),
            'UP':P('PRESS')+' '+K_('S'), 'UPTOWARD':P('PRESS')+' '+K_('S'), 'TILT':P('PRESS')+' '+K_('SPACE'),
            'DOWN':P('PRESS')+' '+K_('CTRL'), 'KNEE':P('PRESS')+' '+K_('CTRL'), 'KNEE_L':P('PRESS')+' '+K_('CTRL+A'),
            'KNEE_R':P('PRESS')+' '+K_('CTRL+D'), 'TWIST':P('PRESS')+' '+K_('Y'), 'WHIP':P('PRESS')+' '+K_('Q')}[g]
def kbm(g,up,t=''):
    if g=='L' and 'drawn' in t and 'grab' not in t: g='WHIP'
    return en_phrase(g,up)
out=[]; bad=0
for s in en:
    t=s.decode('latin1')
    if 'reconnect' in t:
        r=t.replace('the SIXAXIS\xac wireless controller','your controller').replace('SIXAXIS\xac wireless controller','your controller')
        out.append((s,r.encode('latin1'),r.encode('latin1'),'RECONNECT')); continue
    m=re.search(VERB,t)
    if not m: print('NOVERB',repr(t)); bad+=1; continue
    rest=t[m.end():]
    g=None
    for name,rx in DIRS:
        mm=re.match(rx,rest)
        if mm: g=name; break
    if not g: print('NODIR',repr(t)); bad+=1; continue
    verb=m.group(1)
    if verb=='ROTATE' and g=='UP': g='TILT'
    up=verb.isupper()
    a,b=m.start(),m.end()+mm.end()
    styled=t[a:a+3]=='[c='
    pp,kk=pad(g,up),kbm(g,up,t)
    if not styled:
        pp=re.sub(r'\[c=[A-Z0-9_]+\]','',pp); kk=re.sub(r'\[c=[A-Z0-9_]+\]','',kk)
    p=t[:a]+pp+t[b:]
    k=t[:a]+kk+t[b:]
    k=k.replace('[LSTop]','RMB').replace('[RSTop]','LMB')
    if 'low object' in k: k=slam_keys(k)
    out.append((s,p.encode('latin1'),k.encode('latin1'),g))
for s,p,k,g in out:
    ok = len(p)<=len(s) and len(k)<=len(s)
    if not ok: bad+=1
    print(g, len(s),len(p),len(k), 'OK' if ok else 'TOO LONG')
    print('  P:',p.decode('latin1')); print('  K:',k.decode('latin1'))
print('bad',bad,'n',len(out))
pickle.dump(out,open('out.pkl','wb'))
