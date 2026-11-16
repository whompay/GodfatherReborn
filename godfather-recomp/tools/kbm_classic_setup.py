"""kbm_classic_setup.py -- build the keyboard/mouse button icons of both keyboard schemes from the player's own game files.

  assets/gf_kbm_glyphs.bin           "Reborn" (default)
  assets/gf_kbm_glyphs_classic.bin   "PC Classic" (KBM_SCHEME=classic, settings menu "Keyboard Controls")

The installer's kbm-glyphs step does the same; this is for an existing install.
usage (from the godfather-recomp folder, with the installer's Python):
  ..\\.installer\\venv\\Scripts\\python.exe tools\\kbm_classic_setup.py <game USRDIR>
"""
import os, sys
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)

def main(base):
    import gf_kbm_build
    gf_kbm_build.main(base, os.path.join(os.path.dirname(here), 'assets', 'gf_kbm_glyphs.bin'))

if __name__ == '__main__':
    if len(sys.argv) != 2: sys.exit(__doc__)
    main(sys.argv[1])
