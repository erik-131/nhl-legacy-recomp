#!/usr/bin/env python3
"""scrape_variant.py IN OUT VARIANT[,VARIANT]: edit the game's frame script (scrape/default.scrape) for a test.

Works on the user's own copy; never commit a script or its output (EA data, public repo).
  base       unchanged
  noshadow   no shadow map passes outside gameplay, as in gameplay
  noao       no ambient occlusion pass outside gameplay, as in gameplay
  glassnochar  glass reflections without characters
  glassoff   glass reflections cleared but not drawn
  icenocrowd   ice reflection without crowd and translucent
  main1x     main pass at 1x without tiling (the hi-res screenshot path), FXAA stays
"""
import re
import sys


def proc(s, name):
    m = re.search(r'^proc %s\b.*?^endproc' % name, s, re.S | re.M)
    if not m:
        raise SystemExit(f'no proc {name}')
    return m


def edit_proc(s, name, fn):
    m = proc(s, name)
    new = fn(m[0])
    if new == m[0]:
        raise SystemExit(f'{name}: nothing changed')
    return s[:m.start()] + new + s[m.end():]


def strip_lines(pattern):
    return lambda body: re.sub(r'^[ \t]*(%s)[^\n]*\n' % pattern, '', body, flags=re.M)


def noshadow(body):
    return re.sub(r'ifnot renderoptions is_in_gameplay.*?endif', '', body, flags=re.S)


def noao(body):
    return re.sub(r'ifnot renderoptions is_in_gameplay.*?else\s*?\n\s*endif', '', body, count=1, flags=re.S)


def take_else(body):
    """Replace every `ifnot renderoptions taking_hires_screenshot .. else .. endif` by its else branch."""
    pat = re.compile(r'ifnot renderoptions taking_hires_screenshot.*?\belse\b(.*?)\bendif', re.S)
    return pat.sub(lambda m: m[1], body)


def apply(s, v):
    if v == 'base':
        return s
    if v == 'noshadow':
        return edit_proc(s, 'RenderShadows', noshadow)
    if v == 'noao':
        return edit_proc(s, 'RenderSSAO', noao)
    if v in ('glassnochar', 'glassoff'):
        pat = 'dispatchbin (characters|characterparts|misccharacters) ' if v == 'glassnochar' else 'dispatchbin '
        for p in ('RenderGlassReflections1', 'RenderGlassReflections2'):
            s = edit_proc(s, p, strip_lines(pat))
        return s
    if v == 'icenocrowd':
        return edit_proc(s, 'RenderIceReflections', strip_lines('dispatchbin (crowd|translucent) '))
    if v == 'main1x':
        s = edit_proc(s, 'BeginNormalScene', take_else)
        return edit_proc(s, 'EndNormalScene', take_else)
    raise SystemExit(f'unknown variant {v}')


def main():
    data = open(sys.argv[1], newline='').read().replace('\r\n', '\n')
    for v in sys.argv[3].split(','):
        data = apply(data, v)
    with open(sys.argv[2], 'w', newline='') as f:
        f.write(data.replace('\n', '\r\n'))


if __name__ == '__main__':
    main()
