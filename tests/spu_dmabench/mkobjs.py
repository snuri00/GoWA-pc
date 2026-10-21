import re
import sys

build, out = sys.argv[1], sys.argv[2]
text = open(build + '/build.ninja').read()
m = re.search(r'build gowa\.exe: CXX_EXECUTABLE_LINKER__gowa_RelWithDebInfo (.*?) \|', text)
objs = [o for o in m.group(1).split() if 'main.cpp.obj' not in o]
with open(out, 'w') as f:
    f.write('\n'.join('"' + build + '/' + o.replace('\\', '/') + '"' for o in objs))
