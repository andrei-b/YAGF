"""Discover local OCR listeners and expose them through an existing SSH forward."""
import concurrent.futures
import base64
import json
import threading
import time
import urllib.error
import urllib.request

PROVIDERS={'paddleocr','doctr','rapidocr','surya','kraken'}


class CatalogError(Exception):
    def __init__(self,status,body): self.status=status; self.body=body; super().__init__(body.get('error','OCR service failed'))


def get_json(url,timeout=1,limit=256*1024):
    with urllib.request.urlopen(url,timeout=timeout) as response:
        data=response.read(limit+1)
    if len(data)>limit: raise ValueError('Service response exceeds the limit')
    value=json.loads(data)
    if not isinstance(value,dict): raise ValueError('Invalid service response')
    return value


class LocalOcrCatalog:
    def __init__(self,ports=range(8765,8770),vl_port=8082):
        self.ports=tuple(ports); self.vl_port=vl_port
        self.entries={}; self.checked=0; self.lock=threading.Lock()

    def discover(self):
        with self.lock:
            if time.monotonic()-self.checked<15: return list(self.entries.values())
            def probe(port):
                try:
                    root='http://127.0.0.1:'+str(port)
                    health=get_json(root+'/v1/models' if port==self.vl_port else root+'/v1/health')
                    if port==self.vl_port:
                        models=[m.get('id','') for m in health.get('data',[]) if isinstance(m,dict)]
                        model=next((m for m in models if isinstance(m,str) and 'paddleocr-vl' in m.lower()),'')
                        if not model or len(model)>256: return None
                        return dict(port=port,provider='paddleocr',label=model,language='auto',kind='vl',model=model,tasks=['ocr','formula'])
                    provider=health.get('engine_id')
                    if health.get('protocol')!='yagf-ocr/1' or health.get('status')!='ready' or provider not in PROVIDERS: return None
                    tasks=health.get('tasks',['ocr','formula'] if provider=='surya' else ['ocr'])
                    tasks=[task for task in tasks if task in ('ocr','formula')] if isinstance(tasks,list) else ['ocr']
                    return dict(port=port,provider=provider,label=str(health.get('model') or provider)[:128],model=str(health.get('model') or '')[:256],language=str(health.get('language') or 'auto')[:64],kind='ocr',tasks=tasks)
                except (OSError,ValueError,TypeError): return None
            ports=self.ports+((self.vl_port,) if self.vl_port else ())
            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
                self.entries={str(e['port']):e for e in pool.map(probe,ports) if e}
            self.checked=time.monotonic()
            return list(self.entries.values())

    def recognize(self,service_id,request):
        self.discover(); entry=self.entries.get(service_id)
        if not entry: raise ValueError('OCR listener is no longer available; discover services again')
        root='http://127.0.0.1:'+str(entry['port'])
        if entry['kind']=='vl':
            from server import PaddleVlBackend, TranscriptionTruncated, png_size
            png=base64.b64decode(request.get('image_png',''),validate=True)
            if len(png)>32*1024*1024: raise ValueError('PNG image is too large')
            width,height=png_size(png)
            try: lines=PaddleVlBackend(root+'/v1/chat/completions',entry['model']).recognize(png,width,height,'auto',request.get('task','ocr'))
            except TranscriptionTruncated as error: raise CatalogError(400,dict(error=str(error),code='transcription_truncated')) from error
            return dict(protocol='yagf-ocr/1',engine_id='paddleocr',width=width,height=height,language='auto',lines=lines)
        payload=json.dumps(request,ensure_ascii=False).encode()
        call=urllib.request.Request(root+'/v1/recognize',payload,headers={'Content-Type':'application/json'})
        try:
            with urllib.request.urlopen(call,timeout=180) as response: data=response.read(8*1024*1024+1)
        except urllib.error.HTTPError as error:
            try: body=json.loads(error.read(8192))
            except ValueError: body={'error':'Upstream OCR service failed'}
            raise CatalogError(error.code,body if isinstance(body,dict) else {'error':'Upstream OCR service failed'}) from error
        if len(data)>8*1024*1024: raise ValueError('Upstream OCR output is too large')
        result=json.loads(data)
        if not isinstance(result,dict): raise ValueError('Invalid upstream OCR response')
        return result
