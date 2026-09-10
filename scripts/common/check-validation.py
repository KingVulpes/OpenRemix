#!/usr/bin/env python3
"""Reject Vulkan validation errors in Meson's captured test output."""
from pathlib import Path
import re
import sys
log = Path(sys.argv[1]).read_text()
errors = [line for line in log.splitlines() if re.search(r'Validation Error|VUID-', line)]
if errors:
    print('\n'.join(errors))
    raise SystemExit(1)
print('No Vulkan validation errors in captured test output')
