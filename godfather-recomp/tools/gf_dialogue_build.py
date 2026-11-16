"""v1106D: builds assets/gf_dialogue.txt -- the voice lines the street-dialogue menu (gf_dialogue.c) may play.
Source: the disc's audiostreams/*/dx_*.exa.snu (EA SNU, EA-XAS v1 mono 24 kHz) + the English subtitle table
godfather_v4/text/310D3A42.str (gf_global.en.LH2: key = gf_mission_hash(stream base name)). Only lines WITH a subtitle
are used, so the text shown always matches the audio. Categories are hand-picked by text (see CAT below).
usage: python gf_dialogue_build.py <USRDIR> <out.txt>"""
import os, sys, struct, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import gf_str
M = 0xFFFFFFFF
def h(t):
    x = 0
    for c in t.encode('latin1'):
        if 65 <= c <= 90: c += 32
        x = ((x << 6) + (x << 16) - x + c) & M
    return x
def norm(t): return re.sub(r'[^a-z0-9]', '', t.lower())
USR, OUT = sys.argv[1], sys.argv[2]
# 310D3A42 = English table (BLES00043); BLES00047 ships F22B0CC2 (Spanish) instead. Category texts below are English,
# so with a non-English table only the line list is built and the English-keyed categories stay empty.
_T = os.path.join(USR, 'godfather_v4', 'text')
_tp = next((os.path.join(_T, t) for t in ('310D3A42.str', 'F22B0CC2.str') if os.path.exists(os.path.join(_T, t))), None)
if _tp is None: sys.exit('no subtitle table found in ' + _T)
lh2 = next(u for o, u in gf_str.chunks(_tp))
n = struct.unpack('>I', lh2[0xA0:0xA4])[0]
keys = struct.unpack('>%dI' % n, lh2[0xB0:0xB0 + 4 * n]); offs = struct.unpack('>%dI' % n, lh2[0xB0 + 4 * n:0xB0 + 8 * n])
idx = {k: i for i, k in enumerate(keys)}
def sub(i): o = 0x90 + offs[i]; return lh2[o:lh2.index(b'\0', o)].decode('latin1')
lines = []   # (cat_src, speaker, relpath, text)
A = os.path.join(USR, 'audiostreams')
for d in sorted(os.listdir(A)):
    p = os.path.join(A, d)
    if not os.path.isdir(p): continue
    for f in sorted(os.listdir(p)):
        if not f.endswith('.exa.snu'): continue
        b = f[:-8]; k = h(b)
        if k not in idx: continue
        t = sub(idx[k]).strip()
        if len(t) < 2 or t == '_': continue
        parts = b.split('_')
        lines.append((parts[1] + '_' + parts[3], parts[2], d + '/' + f, t))
# category -> (source categories, speakers, exact texts)
G_M, G_F = ['imgx'], ['ifgx']
PR_M = ['ma01','ma02','ma03','ma04','ma05','ma06','ma07','ma08','ma11','ma12','ma13','ma15','ma20']; PR_F = ['fm04','fm06','fm08']
CAT = {
 'HELLO_M': (['chat_repu'], G_M, ["I hear you can be trusted.", "It's so nice to meet someone so friendly", "Hey. It's good talking to you.",
     "You are here to protect us.", "The Corleones make this town better", "You sure can talk the talk, mister!", "What a good samaritan",
     "Everyone's gotta make a living.", "I say, take out all the thugs you want"]),
 'HELLO_F': (['chat_repu'], G_F, ["I am so glad that we have such fine men in New York.", "You are welcome around here.", "Are you here to help us?",
     "You're a good man.", "You take care of this neighborhood", "You are such a sweet talker.", "You're quite the talker ain't ya, mister smooth?",
     "Keep talking Lover-boy, my favorite color is mink.", "It's nice to see people getting along THESE days.", "Hey Mr. Nice Guy, why don't you do ME a favor."]),
 'HURT_M': (['chat_repu'], G_M, ["Hey mac - you don't look so good.  You should find ugh one of them clinics.", "I hope YOU'RE on your way to a hospital.",
     "You should find a doc - he'll fix you up.", "You ain't gonna last long in YOUR condition...", "Why don't you find some place to heal up?",
     "Man, one more shot in you, and you're a goner!", "You better see a doc about them bruises, buddy."]),
 'HURT_F': (['chat_repu'], G_F, ["There's a doc in town that can fix you up, buddy.", "You don't look so good...", "Ugh are you OK?  You don't look too healthy...",
     "My goodness - what happened to you?", "You should stop by a clinic and get fixed up!", "You should really see a doctor about those wounds...",
     "Oh you poor baby, look at that face!", "Put something cold on that bruise, it'll help the swelling."]),
 'GOSSIP_M': (['chat_repu'], G_M, ["Business seems to be going good for you guys.", "So I guess the Corleones own this town now.",
     "Some things just, uh, fall off the back of a truck, right? Ain't no harm there?", "Control the warehouses, control New York - ain't that how it works?",
     "Banks ain't safe around here - I keep MY money under my mattress!", "You're makin' this town a better place to do business.",
     "When you do someone a favor, they won't forget it.", "Ooh, if only someone would do ME a favor.", "Scalesi and Strong - ain't that the name of the armored car company?",
     "Lookin' for a Scalesi and Strong truck?  Look for 'em at the big banks, pal!", "I don't like cops, but at least they keep the peace!",
     "There is no better thug than a dead thug.", "Hey, that shopkeeper is grateful for the favor, mac!", "Did you get this stuff from Freddie the Fence? It'll probably break."]),
 'GOSSIP_F': (['chat_repu'], G_F, ["The blackmarket is the only way to survive", "I hear you know how to get the best deals.", "Buy low, sell high, my husband always says.",
     "The merchants round here never forget a favor.", "I wonder who else needs a favor?", "You lookin' for Scalesi and Strong?  That's the bank truck company.",
     "A bank guard told me that no truck's stronger than a Scalesi and Strong!", "I can't leave my car unlocked for a second!", "This town isn't safe anymore",
     "I won't let my children go out on the streets", "Tell Tessio I said Hi, OK?", "Good thing the corleones are here to clean up this town.",
     "It's so nice of you to help out those shopkeepers!", "You're making this neighborhood a better place to shop."]),
 'PL_THREAT': (['scrp_pres', 'chat_cmbt'], ['plyr'], ["I don't take no for an answer.", "Try me.", "I'm gonna count to 3.", "You holding out on me?",
     "I'm losing my patience.", "This is your last chance.", "You get one warning.", "Don't test me.", "You want me to rough you up?", "Don't make me hurt you.",
     "You're about to have a bad day.", "You fond of your teeth?", "How about I knock your teeth out?", "Don't fuck with me.",
     "I don't think you're listening to me.", "Let me make myself clear.", "I'm not a violent man, unless I have to be.", "I hope you're paying attention.",
     "I'm trying not to get angry.", "You don't want me to get angry.", "I'm trying very hard not to lose my patience with you.", "Are you afraid? You should be.",
     "Your life is in my hands."]),
 'PL_THREAT_GUN': (['scrp_pres', 'chat_cmbt'], ['plyr'], ["Give me what I want or I shoot you.", "I've got an itchy trigger finger.", "Feeling lucky?",
     "Give in or you're dead.", "10 seconds til you die.", "Next shot--you're dead.", "Is there any reason why I shouldn't kill you?", "You wanna die?",
     "Don't make me kill you.", "Your life is in my hands."]),
 'PL_BYE': (['chat_cmbt'], ['plyr'], ["Ciao."]),
 'SCARED_M': (['chat_prss'], PR_M, ["Look, take it easy, calm down...", "Easy mister, calm down...", "Hold it now...just calm down a minute.", "Whoa, just calm down a sec.",
     "HEY...calm down!", "Wait!  Can't we talk about this.", "Hold it, let's talk this through!", "Hold it - we can talk about this...", "Hang on - we can talk about this...",
     "Come on, let's talk this through!", "Don't!  Please don't!", "Leave me alone!", "Damn, what did I do?", "Hey, what did I do?", "What did I do?", "Christ, what did I do?",
     "Cripes, what did I do?", "C'mon - I'm just a working guy!", "Please - I'm just doing my job!", "Don't do it!", "Please don't!", "Hey, mister, calm down...", "Just calm down a sec."]),
 'SCARED_F': (['chat_prss'], PR_F, ["Why?  Why me?", "Please - don't do this...", "I don't want no trouble from you.", "Leave me alone...please!", "Please...just go away!",
     "Don't do  that - let's talk!", "Please - I have children!", "Have mercy!", "No... please... don't do it...", "I'll cooperate, just leave me alone!", "Just - go away, all right?",
     "No - I have kids!", "I don't wanna die!", "What are you gonna do?", "Don't touch me!", "Oh please - NO!", "Give a girl a break!", "Please - don't!", "Don't do this"]),
 'DEFY_M': (['chat_prss'], PR_M, ["You can't muscle me!", "Who do you think you are!?", "Bring it on, tough guy...", "You're gettin' nowhere, pal...", "This will get you nowhere, mack.",
     "You'll be sorry - I got protection!", "You can't do that, I got friends!", "Think you're a tough guy?", "Tough guy, huh?", "I ain't afraid of you, tough guy...",
     "That don't scare ME, buddy...", "Your threats don't scare me - beat it.", "You threats don't scare me - beat it.", "You wanna fight, tough guy?", "Go ahead - I dare ya!",
     "Hey, what's your problem.", "What's the matter with YOU?", "Hey, what's the big idea?", "Damn punk!", "Damn goomba!", "Better start walkin'...", "Now, just get outta here.",
     "Just get out of here, OK fella?", "Just wait til I tell the cops about THIS!", "The cops will hear about THIS!", "This is gettin' ya nowhere...",
     "You'll be sorry - I'm protected!", "You won't get away with this...", "That doesn't scare ME, buddy...", "I ain't afraid of you."]),
 'DEFY_F': (['chat_prss'], PR_F, ["You've got no business here.", "You've gone too far!", "Don't do it - I'll call the cops!", "I'll tell my husband!", "If you touch me, you'll regret it!",
     "Keep your paws to yourself!", "Get outta here, please?", "Don't hit me - I'll call the cops!", "You brute, leave me alone!", "What kinda man ARE you?"]),
 'GUN_M': (['chat_prss'], PR_M, ["Don't shoot!", "Put that gun away!", "Put that away!", "Watch where you're pointing that thing!", "Don't point that thing at me!",
     "What are you gonna do, shoot me?", "You gonna pull the trigger?", "You're not gonna shoot me...", "You don't have the guts...", "I DARE you to shoot me.",
     "You ain't the first goon to pull a gun on me...", "Think you can pull the trigger?", "Put the gun away!", "Watch where you're pointin' that thing!", "I dare you to shoot me."]),
 'GUN_F': (['chat_prss'], PR_F, ["Help!  He's got a GUN!", "Don't shoot...tell me what you want!", "Put the gun away!", "Don't shootjust tell me what you want!", "You wouldn't!"]),
 'GIVEUP_M': (['chat_prss'], PR_M, ["Stop, you win!", "Stop!  I give up!", "Stop, you win!  Have it your way.", "Stop!  We can do business!", "Stop!  I'll pay!", "Stop!  You'll get your money!"]),
 'GIVEUP_F': (['chat_prss'], PR_F, ["I can't take this!  You win.", "Stop it mister, you win!", "I give up, just leave me alone!", "Stop it, you win!", "Stop!  Stop!  I'll pay!", "Stop!  I get it!  I'll pay!"]),
}
out = []; miss = []
for cat, (srcs, spk, texts) in CAT.items():
    want = {norm(t) for t in texts}; got = set()
    for c, s, path, t in lines:
        if c in srcs and s in spk and norm(t) in want:
            out.append((cat, s, path, t)); got.add(norm(t))
    for t in texts:
        if norm(t) not in got: miss.append((cat, t))
os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)   # a fresh install has no assets/ yet
with open(OUT, 'w', encoding='latin1', newline='\n') as f:
    f.write('# gf_dialogue.txt v1106D -- generated by tools/gf_dialogue_build.py: category<TAB>speaker<TAB>audiostreams path<TAB>subtitle\n')
    for r in out: f.write('\t'.join(r) + '\n')
import collections
print(len(out), 'lines'); print(collections.Counter(r[0] for r in out))
print('missing', miss)
