"""Inference helper executed inside a profile's isolated Python environment."""
import argparse
import contextlib
import json
import sys
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(); parser.add_argument('engine',choices=['kraken','paddleocr']); parser.add_argument('model'); parser.add_argument('image'); parser.add_argument('--line',action='store_true'); parser.add_argument('--page',action='store_true'); parser.add_argument('--batch',action='store_true')
    args=parser.parse_args(); paths=json.loads(Path(args.image).read_text()) if args.batch else [args.image]
    results=[]
    # Framework diagnostics must not corrupt the machine-readable result.
    with contextlib.redirect_stdout(sys.stderr):
        from PIL import Image
        if args.engine=='kraken':
            import torch
            torch.set_num_threads(2)
            from kraken.tasks import RecognitionTaskModel
            from kraken.configs import RecognitionInferenceConfig
            from kraken.containers import Segmentation, BBoxLine
            from server import normalize_kraken_records
            model=RecognitionTaskModel.load_model(args.model)
            config=RecognitionInferenceConfig(accelerator='cpu',device=1,num_threads=2,num_line_workers=0)
            for path in paths:
                with Image.open(path) as original: image=original.convert('RGB')
                width,height=image.size
                if args.line:
                    segmentation=Segmentation(type='bbox',imagename=path,text_direction='horizontal-lr',script_detection=False,
                        lines=[BBoxLine(id='line',bbox=(0,0,width,height))])
                else:
                    # Native adaptation changes the line recognizer. Keep page
                    # geometry inexpensive and independent of another neural
                    # layout model, especially for small selected blocks.
                    from kraken import binarization, pageseg
                    segmentation=pageseg.segment(binarization.nlbin(image.convert('L')))
                words=normalize_kraken_records(model.predict(image,segmentation,config),width,height)
                results.append({'text':' '.join(w['text'] for w in words),'words':words})
        else:
            import numpy as np
            if args.line:
                from paddleocr import TextRecognition
                model=TextRecognition(model_dir=args.model,device='cpu')
            else:
                from paddleocr import PaddleOCR
                from server import normalize_paddle_result
                model=PaddleOCR(text_recognition_model_dir=args.model,device='cpu',use_doc_orientation_classify=False,use_doc_unwarping=False,use_textline_orientation=False)
            for path in paths:
                with Image.open(path) as original: image=original.convert('RGB')
                width,height=image.size
                if args.line:
                    outputs=list(model.predict(np.asarray(image))); values=[r.json.get('res',r.json) for r in outputs]
                    result={'text':' '.join(v.get('rec_text','') for v in values),'words':[]}
                else:
                    lines=normalize_paddle_result(list(model.predict(np.asarray(image))),width,height)
                    words=[dict(line,block=index+1,line=1,layout_block=True) for index,line in enumerate(lines)]
                    result={'text':'\n'.join(w['text'] for w in words),'words':words}
                results.append(result)
    output={'predictions':[r['text'] for r in results]} if args.batch else results[0]
    print(json.dumps(output,ensure_ascii=False,allow_nan=False))

if __name__=='__main__': main()
