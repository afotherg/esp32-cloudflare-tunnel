"""Embed the self-contained page in flash, without a runtime filesystem."""
from pathlib import Path
import gzip

Import('env')
root = Path(env['PROJECT_DIR'])
page = (root / 'web/dashboard.html').read_text()
assert ')DASHBOARD"' not in page
header = '#pragma once\nstatic constexpr char DASHBOARD_HTML[] = R"DASHBOARD(' + page + ')DASHBOARD";\n'
compressed = gzip.compress(page.encode(), compresslevel=9, mtime=0)
header += 'static constexpr unsigned char DASHBOARD_GZIP[] = {' + ','.join(str(b) for b in compressed) + '};\n'
target = root / 'src/dashboard_asset.hpp'
if not target.exists() or target.read_text() != header:
    target.write_text(header)
