#!/usr/bin/env python3
"""report-data.json + template + content -> report/fable-stres.html"""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.environ.get("FABLE_RUNS", os.path.expanduser("~/fable-runs"))
data = json.load(open(os.path.join(R, 'report-data.json')))
tpl = open(os.path.join(HERE, 'report_template.html')).read()
content = open(os.path.join(HERE, 'report_content.js')).read()
out = tpl.replace('const D = /*DATA*/null;', 'const D = ' + json.dumps(data) + ';\n' + content)
assert out != tpl
open(os.path.join(R, 'fable-stres.html'), 'w').write(out)
print('written report/fable-stres.html', len(out))
