"""Build the standalone USB Android Auto application; never runs target code."""
import argparse,json,os,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc',default=os.environ.get('CC','clang'))
p.add_argument('--ld',default=os.environ.get('LD_LLD','ld.lld'))
p.add_argument('--out',type=Path,default=ROOT/'build')
a=p.parse_args()
required=['vendor','runtime/aa/scene/winston','runtime/aa/scene/libwlxs-genivi.so','runtime/aa/identity/phone.crt','runtime/aa/identity/phone.key','assets/OwnAndroidAuto_Images.zip']
missing=[name for name in required if not (ROOT/name).exists()]
if missing:raise SystemExit('Missing local build inputs: '+', '.join(missing)+'; see PRIVATE_INPUTS_RU.md')
out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
shutil.copytree(ROOT/'runtime/aa',out/'aa',dirs_exist_ok=True,
                ignore=shutil.ignore_patterns('.DS_Store','__pycache__','*.pyc'))
for name in ('OwnAndroidAuto.xml','OwnAndroidAuto.manifest.in','OwnAndroidAuto_Images.zip','OwnAndroidAuto_Texts.zip'):
    shutil.copy2(ROOT/'assets'/name,out/name)
log=[]
for args in json.loads((ROOT/'commands.json').read_text()):
    argv=[a.cc]+[v.replace('${ROOT}',str(ROOT)).replace('${OUT}',str(out)).replace('${LD}',a.ld) for v in args]
    r=subprocess.run(argv,cwd=ROOT,capture_output=True,text=True)
    log.append(dict(argv=[Path(a.cc).name]+args,exit=r.returncode,stdout=r.stdout.replace(str(ROOT),".").replace(str(out),"${OUT}"),stderr=r.stderr.replace(str(ROOT),".").replace(str(out),"${OUT}")))
    (out/'compile.json').write_text(json.dumps(log,indent=2)+'\n')
    if r.returncode:raise SystemExit(r.stderr)
shutil.copy2(ROOT/'install-usb.sh',out/'run-hu.sh')
shutil.copy2(out/'OwnAndroidAuto.manifest.in',out/'OwnAndroidAuto.manifest')
print(out)
