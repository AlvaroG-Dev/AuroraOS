p='scripts/qemu-drive.py'
s=open(p).read()
s=s.replace("a = ap.parse_args()", "ap.add_argument('--no-cdrom', action='store_true', help='no conectar aurora.iso como CD (arranque solo desde el USB, como en un PC sin lector)')\nap.add_argument('--no-build', action='store_true', help='no ejecutar make iso (usa aurora.iso tal cual)')\na = ap.parse_args()",1)
s=s.replace("r = subprocess.run(['make', 'iso',", "r = subprocess.run(['true'] if a.no_build else ['make', 'iso',",1)
s=s.replace("'-m', a.m, '-no-reboot', '-smp', '2', '-cdrom', 'aurora.iso',", "'-m', a.m, '-no-reboot', '-smp', '2'] + ([] if a.no_cdrom else ['-cdrom', 'aurora.iso']) + [",1)
open(p,'w').write(s)
