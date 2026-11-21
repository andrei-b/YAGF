"""Local native OCR training host with reviewed datasets and immutable model versions."""
import argparse
import base64
import binascii
import hashlib
import hmac
import json
import math
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile
import threading
import time
import unicodedata
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import native_engines
from ocr_catalog import LocalOcrCatalog, CatalogError

PROTOCOL='yagf-native-training/1'
ID=re.compile(r'^[a-f0-9-]{36}$')


def png_bytes(encoded):
    if not isinstance(encoded,str) or len(encoded)>44*1024*1024: raise ValueError('Invalid PNG data')
    data=base64.b64decode(encoded,validate=True)
    if len(data)<33 or data[:8]!=b'\x89PNG\r\n\x1a\n' or data[12:16]!=b'IHDR': raise ValueError('Invalid PNG image')
    width,height=struct.unpack('>II',data[16:24])
    if not 1<=width<=20000 or not 1<=height<=20000 or width*height>80000000: raise ValueError('Image dimensions exceed the limit')
    return data,width,height


def validate_samples(items):
    if not isinstance(items,list) or not 3<=len(items)<=5000: raise ValueError('Approve at least three distinct examples, including training and validation data')
    ids=set(); images=set(); samples=[]; size=0; counts={'train':0,'validation':0}; pages={}
    for item in items:
        if not isinstance(item,dict) or item.get('decision')!='approved': raise ValueError('Only explicitly approved examples may be trained')
        sid=item.get('id'); text=item.get('text'); partition=item.get('partition'); page=item.get('page')
        if not isinstance(sid,str) or not 1<=len(sid)<=128 or sid in ids: raise ValueError('Invalid or repeated sample ID')
        if not isinstance(text,str) or not 1<=len(text)<=4096 or text.strip()!=text or any(not c.isprintable() or (c.isspace() and c!=' ') for c in text): raise ValueError('Each transcription must be one printable line')
        if partition not in counts or type(page) is not int or not 1<=page<=100000: raise ValueError('Invalid training partition or source page')
        data,width,height=png_bytes(item.get('png_base64'))
        size+=len(data)
        if len(data)>2*1024*1024 or size>32*1024*1024 or width>16000 or height>4000 or width*height>8000000: raise ValueError('Training examples exceed image limits')
        digest=hashlib.sha256(data).hexdigest()
        if digest in images: raise ValueError('Repeated images must be removed; training/validation duplicates are not allowed')
        images.add(digest); ids.add(sid); counts[partition]+=1; pages.setdefault(page,set()).add(partition)
        samples.append(dict(item,text=unicodedata.normalize('NFC',text),image_hash=digest))
    if counts['train']<2 or counts['validation']<1: raise ValueError('Approve at least two training examples and one validation example')
    if len(pages)>1 and any(len(parts)>1 for parts in pages.values()): raise ValueError('For multi-page datasets, keep each page entirely in training or validation')
    return samples


def distance(a,b):
    previous=list(range(len(b)+1))
    for i,char in enumerate(a,1):
        current=[i]
        for j,other in enumerate(b,1): current.append(min(current[-1]+1,previous[j]+1,previous[j-1]+(char!=other)))
        previous=current
    return previous[-1]


def normalized(text): return ' '.join(unicodedata.normalize('NFC',text).split())


def atomic_json(path,value):
    temporary=path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value,ensure_ascii=False,allow_nan=False,indent=2),encoding='utf-8')
    temporary.replace(path)


class TrainingHost:
    def __init__(self,root,profiles):
        self.root=Path(root).resolve(); self.root.mkdir(parents=True,exist_ok=True)
        self.profiles={p['id']:p for p in profiles}; self.lock=threading.Lock(); self.active=None; self.cancel={}; self.state_lock=threading.RLock()
        self.catalog=LocalOcrCatalog(); self.inference_slots=threading.BoundedSemaphore(2)
        # A stopped/restarted host cannot silently report an orphaned run as active.
        for state in self.root.glob('*/state.json'):
            data=json.loads(state.read_text())
            if data.get('status') in ('queued','running'):
                data.update(status='failed',message='Training host restarted; start a new run from the reviewed dataset'); atomic_json(state,data)

    def capabilities(self):
        profiles=[]
        for p in self.profiles.values():
            ready,reason=native_engines.readiness(p)
            profiles.append({'id':p['id'],'engine':p['engine'],'label':p.get('label',p['id']),'ready':ready,'reason':reason,
                             'base_model':Path(p.get('base_model','')).name})
        return {'protocol':PROTOCOL,'profiles':profiles,'max_samples':5000,'max_png_bytes':32*1024*1024}

    def health(self):
        services=[]
        for profile in self.capabilities()['profiles']:
            if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}',profile['id']): continue
            original=self.profiles[profile['id']]
            services.append(dict(path='/profiles/'+profile['id'],provider='native',label=profile['label'],language=original.get('language','auto'),ready=profile['ready'],reason=profile['reason']))
        for path in self.root.glob('*/state.json'):
            state=self.state(path.parent.name)
            if state.get('status')=='complete' and state.get('eligible') and state.get('scope')=='shared':
                services.append(dict(path=state['model_url'],provider='native',label=state['label']+' · '+state['id'][:8],language='auto',ready=True))
                if len(services)>=64: break
        for entry in self.catalog.discover():
            if 'profiles' in entry:
                for profile in entry['profiles']:
                    model=profile.get('model')
                    if not isinstance(model,str) or not model or len(model)>256: continue
                    services.append(dict(path='/services/'+str(entry['port']),provider=entry['provider'],label=model[:128],
                                         model=model,language=entry['language'],ready=True,profile=profile))
                continue
            services.append(dict(path='/services/'+str(entry['port']),provider=entry['provider'],label=entry['label'],model=entry.get('model',entry['label']),language=entry['language'],ready=True,tasks=entry.get('tasks',['ocr'])))
            if 'profile' in entry: services[-1]['profile']=entry['profile']
        return dict(protocol='yagf-ocr/1',engine_id='native',status='ready',language='auto',services=services[:64])

    def recognize_profile(self,profile_id,request):
        profile=self.profiles.get(profile_id)
        if not profile: raise ValueError('Unknown OCR profile')
        ready,reason=native_engines.readiness(profile)
        if not ready: raise ValueError(reason)
        png,width,height=png_bytes(request.get('image_png'))
        with self.inference_slots,tempfile.TemporaryDirectory(dir=self.root) as temporary:
            directory=Path(temporary); image=directory/'page.png'; image.write_bytes(png)
            words=native_engines.recognize(profile,profile.get('base_inference_model',profile['base_model']),image,self.runner(directory,threading.Event(),180))
        return dict(protocol='yagf-ocr/1',engine_id='native',width=width,height=height,language=profile.get('language','auto'),words=words)

    def directory(self,jid):
        if not isinstance(jid,str) or not ID.fullmatch(jid): raise ValueError('Invalid model/job ID')
        directory=self.root/jid
        if not (directory/'state.json').is_file(): raise ValueError('Unknown model/job')
        return directory

    def state(self,jid):
        with self.state_lock: return json.loads((self.directory(jid)/'state.json').read_text())

    def update(self,jid,**fields):
        with self.state_lock:
            path=self.directory(jid)/'state.json'; data=json.loads(path.read_text()); data.update(fields); atomic_json(path,data)

    def submit(self,request):
        profile=self.profiles.get(request.get('profile_id'))
        if not profile: raise ValueError('Choose a configured native training profile')
        ready,reason=native_engines.readiness(profile)
        if not ready: raise ValueError(reason)
        source=request.get('source_id'); scope=request.get('scope','project'); iterations=request.get('iterations',100)
        if not isinstance(source,str) or not re.fullmatch('[a-f0-9]{64}',source) or scope not in ('project','shared') or type(iterations) is not int or not 1<=iterations<=10000: raise ValueError('Invalid training configuration')
        samples=validate_samples(request.get('samples'))
        base_id=request.get('base_model_id')
        if base_id:
            previous=self.state(base_id)
            if previous.get('status')!='complete' or not previous.get('eligible') or previous.get('profile_id')!=profile['id']:
                raise ValueError('Choose a validated model from the same native engine profile')
            if previous.get('scope')!='shared' and previous.get('source_id')!=source:
                raise ValueError('This base model belongs to another book and is not reusable')
            # A model has already seen its ancestors' training examples. Those
            # images cannot become held-out validation data in a later version.
            validation_hashes={s['image_hash'] for s in samples if s['partition']=='validation'}
            ancestor=base_id; visited=set()
            while ancestor:
                if ancestor in visited: raise ValueError('Invalid base model ancestry')
                visited.add(ancestor)
                prior=json.loads((self.directory(ancestor)/'dataset.json').read_text())
                trained={s['image_hash'] for s in prior['samples'] if s['partition']=='train'}
                if validation_hashes & trained:
                    raise ValueError('Validation includes images used to train the selected model; choose unseen validation examples')
                ancestor=self.state(ancestor).get('base_model_id')
            profile=dict(profile)
            profile['base_model']=previous['model_path']
            if profile['engine']=='paddleocr':
                profile['base_inference_model']=previous['model_path']
                profile['base_model']=str(self.directory(base_id)/'candidate/best_accuracy.pdparams')
        if not self.lock.acquire(blocking=False): raise ValueError('A training job is already running on this host')
        try:
            jid=str(uuid.uuid4()); directory=self.root/jid; directory.mkdir(); (directory/'images').mkdir()
            for i,sample in enumerate(samples):
                png,_,_=png_bytes(sample['png_base64']); path=directory/'images'/('%06d.png'%i); path.write_bytes(png); path.with_suffix('.gt.txt').write_text(sample['text']+'\n',encoding='utf-8')
            for partition,name in [('train','train.txt'),('validation','validation.txt')]:
                paths=[str(directory/'images'/('%06d.png'%i)) for i,s in enumerate(samples) if s['partition']==partition]
                (directory/name).write_text('\n'.join(paths)+'\n')
                (directory/('paddle-'+name)).write_text(''.join('images/%06d.png\t%s\n'%(i,s['text']) for i,s in enumerate(samples) if s['partition']==partition),encoding='utf-8')
            fingerprint=hashlib.sha256(json.dumps(samples,sort_keys=True,ensure_ascii=False).encode()).hexdigest()
            atomic_json(directory/'profile.json',profile)
            atomic_json(directory/'dataset.json',{'protocol':PROTOCOL,'source_id':source,'samples':samples})
            atomic_json(directory/'state.json',{'protocol':PROTOCOL,'id':jid,'status':'queued','message':'Queued',
                'profile_id':profile['id'],'engine':profile['engine'],'label':profile.get('label',profile['id']),
                'source_id':source,'scope':scope,'dataset_hash':fingerprint,'samples':len(samples),'created_at':time.time()})
            self.update(jid,base_model_id=base_id or '')
            event=threading.Event(); self.cancel[jid]=event; self.active=jid
            threading.Thread(target=self.work,args=(jid,profile,samples,iterations,event),daemon=True).start()
            return self.state(jid)
        except Exception:
            self.lock.release(); raise

    def runner(self,directory,event,timeout=3600):
        def run(args,cwd=None):
            if event.is_set(): raise InterruptedError('Cancelled')
            env=dict(os.environ,OMP_THREAD_LIMIT='2',OMP_NUM_THREADS='2',OPENBLAS_NUM_THREADS='2',MKL_NUM_THREADS='2',TOKENIZERS_PARALLELISM='false',CUDA_VISIBLE_DEVICES='',PYTHONDONTWRITEBYTECODE='1')
            with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
                process=subprocess.Popen(args,cwd=cwd,env=env,stdout=output,stderr=errors,start_new_session=True)
                started=time.monotonic()
                while process.poll() is None:
                    if event.wait(.1) or time.monotonic()-started>timeout:
                        os.killpg(process.pid,signal.SIGTERM)
                        try: process.wait(3)
                        except subprocess.TimeoutExpired: os.killpg(process.pid,signal.SIGKILL); process.wait()
                        raise InterruptedError('Cancelled') if event.is_set() else RuntimeError('Native training command timed out')
                output.seek(0); errors.seek(0); stdout=output.read(); stderr=errors.read()
                with (directory/'training.log').open('ab') as log:
                    log.write(('\n$ '+Path(args[0]).name+' '+(' '.join(args[1:]))+'\n').encode()); log.write(stdout[-1024*1024:]); log.write(stderr[-1024*1024:])
                if process.returncode: raise RuntimeError((stderr or stdout)[-6000:].decode('utf-8',errors='replace') or 'Native trainer failed')
                if len(stdout)>16*1024*1024: raise RuntimeError('Native engine returned oversized output')
                return stdout.decode('utf-8',errors='replace')
        return run

    def evaluate(self,profile,model,directory,samples,run):
        errors=characters=0; predictions=[]
        validation=[(i,s) for i,s in enumerate(samples) if s['partition']=='validation']
        readings=native_engines.recognize_many(profile,model,[directory/'images'/('%06d.png'%i) for i,s in validation],run,directory)
        for (i,sample),prediction in zip(validation,readings):
            truth=normalized(sample['text'])
            prediction=normalized(prediction); count=distance(truth,prediction); errors+=count; characters+=len(truth)
            if len(predictions)<64: predictions.append({'id':sample['id'],'truth':truth,'prediction':prediction,'errors':count})
        if not characters: raise ValueError('Validation transcriptions contain no characters')
        return {'cer':errors/characters,'errors':errors,'characters':characters,'predictions':predictions}

    def work(self,jid,profile,samples,iterations,event):
        directory=self.directory(jid); run=self.runner(directory,event)
        progress=lambda text:self.update(jid,message=text,status='running')
        try:
            progress('Evaluating the original model on validation examples')
            baseline=self.evaluate(profile,profile.get('base_inference_model',profile['base_model']),directory,samples,run); self.update(jid,baseline=baseline)
            model=native_engines.train(profile,directory,samples,iterations,run,progress)
            progress('Evaluating the trained model on the same validation examples')
            candidate=self.evaluate(profile,model,directory,samples,run)
            if event.is_set(): raise InterruptedError('Cancelled')
            self.update(jid,status='complete',message='Training and validation complete',candidate=candidate,
                        model_path=str(model),eligible=candidate['cer']<=baseline['cer'],
                        model_url='/models/'+jid)
        except InterruptedError: self.update(jid,status='cancelled',message='Training cancelled; the active OCR model was not changed')
        except Exception as error: self.update(jid,status='failed',message=str(error)[-6000:])
        finally: self.active=None; self.lock.release()

    def recognize(self,jid,request):
        state=self.state(jid)
        if state['status']!='complete' or not state.get('eligible'): raise ValueError('This candidate is not validated for activation')
        directory=self.directory(jid); profile=json.loads((directory/'profile.json').read_text())
        png,width,height=png_bytes(request.get('image_png'))
        with self.inference_slots,tempfile.TemporaryDirectory(dir=directory) as temporary:
            path=Path(temporary)/'page.png'; path.write_bytes(png)
            words=native_engines.recognize(profile,state['model_path'],path,self.runner(directory,threading.Event(),300))
        return {'protocol':'yagf-ocr/1','engine_id':'native','width':width,'height':height,'language':profile.get('language','auto'),'words':words,
                'native_engine':state['engine'],'native_model':jid}


def handler_for(host,token=''):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*_): pass
        def setup(self): super().setup(); self.connection.settimeout(120)
        def send(self,status,value):
            value.setdefault('protocol',PROTOCOL); data=json.dumps(value,ensure_ascii=False,allow_nan=False).encode()
            self.send_response(status); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(data))); self.end_headers()
            try: self.wfile.write(data)
            except (BrokenPipeError,ConnectionResetError): pass
        def authorized(self):
            if token and not hmac.compare_digest(self.headers.get('Authorization',''),'Bearer '+token): self.send(401,{'error':'Unauthorized'}); return False
            return True
        def do_GET(self):
            if not self.authorized(): return
            try:
                if self.path=='/v1/capabilities': self.send(200,host.capabilities())
                elif self.path=='/v1/health':
                    health=host.health(); health['requires_token']=bool(token); self.send(200,health)
                elif re.fullmatch(r'/(?:profiles/[A-Za-z0-9_-]{1,64}|models/[a-f0-9-]{36}|services/[0-9]{1,5})/v1/health',self.path):
                    kind,identity=self.path.split('/')[1:3]
                    if kind=='profiles':
                        profile=host.profiles.get(identity)
                        if not profile: raise ValueError('Unknown OCR profile')
                        ready,reason=native_engines.readiness(profile)
                        self.send(200,dict(protocol='yagf-ocr/1',engine_id='native',status='ready' if ready else 'unavailable',language=profile.get('language','auto'),model=profile.get('label',identity),reason=reason))
                    elif kind=='models':
                        state=host.state(identity)
                        self.send(200,dict(protocol='yagf-ocr/1',engine_id='native',status='ready' if state.get('eligible') and state['status']=='complete' else 'unavailable',language='auto',model=state.get('label',identity)))
                    else:
                        entry=next((e for e in host.catalog.discover() if str(e['port'])==identity),None)
                        if not entry: raise ValueError('OCR listener is no longer available')
                        health=dict(protocol='yagf-ocr/1',engine_id=entry['provider'],status='ready',language=entry['language'],model=entry.get('model',entry['label']),tasks=entry.get('tasks',['ocr']))
                        if 'profile' in entry: health['profile']=entry['profile']
                        if 'profiles' in entry:
                            health.pop('model',None)
                            health['profiles']=entry['profiles']
                        self.send(200,health)
                elif self.path.startswith('/v1/jobs/'): self.send(200,host.state(self.path.rsplit('/',1)[-1]))
                elif self.path=='/v1/models':
                    models=[]
                    for path in host.root.glob('*/state.json'):
                        state=host.state(path.parent.name)
                        if state.get('status')=='complete' and state.get('eligible'):
                            summary={key:state[key] for key in ('id','status','scope','source_id','engine','label','eligible','model_url','profile_id','samples')}
                            summary['baseline']={'cer':state['baseline']['cer']}; summary['candidate']={'cer':state['candidate']['cer']}
                            models.append(summary)
                    self.send(200,{'models':models})
                else: self.send(404,{'error':'Unknown endpoint'})
            except (ValueError,KeyError) as error: self.send(400,{'error':str(error)})
        def do_POST(self):
            if not self.authorized(): return
            try:
                size=int(self.headers.get('Content-Length','-1'))
                if not 0<size<=48*1024*1024 or self.headers.get_content_type()!='application/json': raise ValueError('Invalid JSON request size or content type')
                request=json.loads(self.rfile.read(size))
                if not isinstance(request,dict): raise ValueError('Invalid JSON request')
                if self.path=='/v1/jobs':
                    if request.get('protocol')!=PROTOCOL: raise ValueError('Incompatible training protocol')
                    self.send(202,host.submit(request))
                elif re.fullmatch(r'/v1/jobs/[a-f0-9-]{36}/cancel',self.path):
                    jid=self.path.split('/')[3]; host.directory(jid)
                    event=host.cancel.get(jid)
                    if event: event.set()
                    self.send(200,host.state(jid))
                elif re.fullmatch(r'/models/[a-f0-9-]{36}/v1/recognize',self.path):
                    if request.get('protocol')!='yagf-ocr/1': raise ValueError('Incompatible OCR protocol')
                    self.send(200,host.recognize(self.path.split('/')[2],request))
                elif re.fullmatch(r'/profiles/[A-Za-z0-9_-]{1,64}/v1/recognize',self.path):
                    if request.get('protocol')!='yagf-ocr/1': raise ValueError('Incompatible OCR protocol')
                    self.send(200,host.recognize_profile(self.path.split('/')[2],request))
                elif re.fullmatch(r'/services/[0-9]{1,5}/v1/recognize',self.path):
                    if request.get('protocol')!='yagf-ocr/1': raise ValueError('Incompatible OCR protocol')
                    self.send(200,host.catalog.recognize(self.path.split('/')[2],request))
                else: self.send(404,{'error':'Unknown endpoint'})
            except CatalogError as error: self.send(error.status,error.body)
            except (ValueError,KeyError,TypeError,binascii.Error) as error: self.send(400,{'error':str(error)})
            except Exception as error: self.send(500,{'error':str(error)[-6000:]})
    return Handler


def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('--config',type=Path,required=True); parser.add_argument('--host',default='127.0.0.1'); parser.add_argument('--port',type=int,default=8772)
    args=parser.parse_args(); config=json.loads(args.config.read_text()); token=os.environ.get('YAGF_NATIVE_TOKEN','')
    if args.host not in ('127.0.0.1','::1','localhost') and not token: parser.error('Non-loopback binding requires YAGF_NATIVE_TOKEN')
    host=TrainingHost(config['storage'],config['profiles']); ThreadingHTTPServer((args.host,args.port),handler_for(host,token)).serve_forever()

if __name__=='__main__': main()
