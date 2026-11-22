"""Engine-native training recipes. Paths come only from the administrator's profiles."""
import csv
import io
import json
from pathlib import Path
import shutil
import unicodedata

TESS_TOOLS = ('tesseract', 'lstmtraining', 'combine_tessdata', 'unicharset_extractor', 'merge_unicharsets', 'combine_lang_model')


def readiness(profile):
    engine = profile.get('engine')
    if not Path(profile.get('base_model', '')).is_file():
        return False, 'Configure an existing trainable base model on this host'
    if engine == 'tesseract':
        missing = [tool for tool in TESS_TOOLS if not shutil.which(tool)]
        return (False, 'Missing native tools: '+', '.join(missing)) if missing else (True, '')
    if engine == 'kraken':
        return (True, '') if Path(profile.get('ketos', '')).is_file() and Path(profile.get('python', '')).is_file() else (False, 'Configure a Kraken training environment')
    if engine == 'paddleocr':
        repo = Path(profile.get('repository', ''))
        ready = Path(profile.get('python', '')).is_file() and (repo/'tools/train.py').is_file() and Path(profile.get('config', '')).is_file() and Path(profile.get('base_inference_model','')).is_dir()
        return (True, '') if ready else (False, 'Configure PaddleOCR source, training configuration and training checkpoint; GGUF/ONNX inference weights cannot be fine-tuned directly')
    return False, 'Unsupported training engine'


def tesseract_words(output):
    words = []
    previous = None
    line_number = 0
    block_number = 0
    for row in csv.DictReader(io.StringIO(output), delimiter='\t'):
        if row.get('level') != '5' or not row.get('text', '').strip():
            continue
        key = (int(row['block_num']), int(row['par_num']), int(row['line_num']))
        if previous is None or key[0] != previous[0]: block_number += 1; line_number = 0
        if key != previous: line_number += 1
        previous = key
        confidence = float(row['conf'])
        if confidence < 0: continue
        words.append({'text': row['text'].strip(), 'confidence': min(1., confidence/100.),
                      'box': [int(row[k]) for k in ('left','top','width','height')], 'block': block_number, 'line': line_number})
    return words


def recognize(profile, model, image, run, line=False):
    engine = profile['engine']
    if engine == 'tesseract':
        model = Path(model)
        args = ['tesseract', str(image), 'stdout', '--tessdata-dir', str(model.parent), '-l', model.stem, '--psm', '7' if line else '3']
        if not line: args.extend(['-c','tessedit_create_tsv=1'])
        output = run(args)
        if line: return output.strip()
        words=tesseract_words(output)
        # Selected blocks can be tightly cropped single lines that automatic
        # page segmentation rejects. Retry only an otherwise empty result.
        if not words:
            args[args.index('--psm')+1]='6'
            words=tesseract_words(run(args))
        return words
    args = [profile['python'], str(Path(__file__).with_name('native_engine_runner.py')), engine,
            str(model), str(image), '--line' if line else '--page']
    output = run(args)
    result = json.loads(output)
    return result['text'] if line else result['words']


def recognize_many(profile, model, images, run, directory):
    if profile['engine']=='tesseract':
        return [recognize(profile,model,path,run,line=True) for path in images]
    manifest=directory/'validation-images.json'; manifest.write_text(json.dumps([str(p) for p in images]))
    output=run([profile['python'],str(Path(__file__).with_name('native_engine_runner.py')),profile['engine'],str(model),str(manifest),'--line','--batch'])
    predictions=json.loads(output).get('predictions')
    if not isinstance(predictions,list) or len(predictions)!=len(images) or any(not isinstance(p,str) for p in predictions):
        raise ValueError('Native engine returned invalid validation predictions')
    return predictions


def train_tesseract(profile, directory, samples, iterations, run, progress):
    base = Path(profile['base_model']); work = directory/'tesseract'; work.mkdir()
    extracted = work/'base.'
    run(['combine_tessdata','-u',str(base),str(extracted)])
    original = (work/'base.lstm-unicharset').read_text(encoding='utf-8').splitlines()[1:]
    known = set(' ')
    for row in original:
        token = row.split(' ',1)[0]
        if token not in ('NULL','Joined','Broken'): known.update(token)
    required = set(''.join(s['text'] for s in samples if s['partition']=='train'))
    proto = base
    if required-known:
        progress('Extending the native Tesseract character set')
        corpus = work/'approved.gt.txt'
        corpus.write_text('\n'.join(s['text'] for s in samples if s['partition']=='train')+'\n',encoding='utf-8')
        chars = work/'new.unicharset'; merged = work/'unicharset'
        run(['unicharset_extractor','--output_unicharset',str(chars),'--norm_mode','2',str(corpus)])
        run(['merge_unicharsets',str(work/'base.lstm-unicharset'),str(chars),str(merged)])
        run(['combine_lang_model','--input_unicharset',str(merged),'--script_dir',profile.get('langdata',str(work)),
             '--output_dir',str(work),'--lang','native'])
        proto = work/'native/native.traineddata'
    train, validation = [], []
    for index, sample in enumerate(samples):
        progress('Preparing native Tesseract examples %d/%d' % (index+1,len(samples)))
        image = directory/'images'/('%06d.png'%index)
        from PIL import Image
        with Image.open(image) as im: width,height=im.size
        label=unicodedata.normalize('NFC',sample['text'])
        # tesstrain's line-box representation: each grapheme has the entire line
        # box, followed by a tab end marker. LSTM learns the alignment itself.
        clusters=[]
        for char in label:
            if unicodedata.combining(char) and clusters: clusters[-1]+=char
            else: clusters.append(char)
        box=image.with_suffix('.box')
        box.write_text(''.join('%s 0 0 %d %d 0\n'%(c,width,height) for c in clusters)+'\t 0 0 %d %d 0\n'%(width,height),encoding='utf-8')
        prefix=image.with_suffix('')
        run(['tesseract',str(image),str(prefix),'--psm','13','lstm.train'])
        path=str(prefix)+'.lstmf'
        if not Path(path).is_file(): raise ValueError('Tesseract did not create a training example')
        (validation if sample['partition']=='validation' else train).append(path)
    for name, paths in [('train.list',train),('validation.list',validation)]:
        (work/name).write_text('\n'.join(paths)+'\n')
    progress('Training the native Tesseract LSTM')
    prefix=work/'weights'
    run(['lstmtraining','--continue_from',str(work/'base.lstm'),'--old_traineddata',str(base),'--traineddata',str(proto),
         '--model_output',str(prefix),'--train_listfile',str(work/'train.list'),'--eval_listfile',str(work/'validation.list'),
         '--max_iterations',str(iterations),'--learning_rate','0.0001','--target_error_rate','0','--debug_interval','0'])
    checkpoint=work/'weights_checkpoint'
    if not checkpoint.is_file():
        checkpoints=list(work.glob('weights*.checkpoint'))
        if not checkpoints: raise ValueError('Tesseract produced no checkpoint')
        checkpoint=max(checkpoints,key=lambda p:p.stat().st_mtime)
    model=directory/'candidate/native.traineddata'; model.parent.mkdir()
    run(['lstmtraining','--stop_training','--continue_from',str(checkpoint),'--traineddata',str(proto),'--model_output',str(model)])
    return model


def train_kraken(profile, directory, samples, iterations, run, progress):
    # Kraken 7's trainer consumes Arrow/XML datasets even though some CLI
    # versions still advertise direct path-mode training. Compile the reviewed
    # line strips first, preserving the explicit held-out split.
    progress('Compiling Kraken native training and validation datasets')
    for partition,name in [('train','train'),('validation','validation')]:
        arrow=directory/(name+'.arrow')
        run([profile['ketos'],'--device','cpu','--workers','0','--threads','2','compile','--format-type','path','--force-type','baseline',
             '--files',str(directory/(name+'.txt')),'--output',str(arrow)])
        (directory/(name+'-arrow.txt')).write_text(str(arrow)+'\n')
    progress('Training Kraken with native codec expansion')
    output=directory/'candidate'
    run([profile['ketos'],'--device','cpu','--workers','0','--threads','2','train','--load',profile['base_model'],'--resize','union','--format-type','binary',
         '--training-data',str(directory/'train-arrow.txt'),'--evaluation-data',str(directory/'validation-arrow.txt'),
         '--quit','fixed','--epochs',str(min(iterations,100)),'--weights-format','safetensors','--output',str(output)])
    models=list(output.rglob('*.safetensors'))
    if not models: raise ValueError('Kraken produced no deployable safetensors model')
    preferred=[p for p in models if 'best' in p.name]
    return max(preferred or models,key=lambda p:p.stat().st_mtime)


def train_paddle(profile, directory, samples, iterations, run, progress):
    progress('Fine-tuning the native PaddleOCR text recognizer')
    output=directory/'candidate'; repo=Path(profile['repository'])
    # Keep the base vocabulary order; append new characters. Paddle's training
    # code derives the output-head size and loads compatible pretrained layers.
    chars=[]
    if profile.get('dictionary'):
        chars=Path(profile['dictionary']).read_text(encoding='utf-8').splitlines()
    for s in samples:
        if s['partition']=='train':
            for char in s['text']:
                if char!=' ' and char not in chars: chars.append(char)
    dictionary=directory/'characters.txt'; dictionary.write_text('\n'.join(chars)+'\n',encoding='utf-8')
    overrides=['Global.pretrained_model='+profile['base_model'],'Global.save_model_dir='+str(output),
               'Global.epoch_num='+str(min(iterations,100)),'Global.use_gpu=False','Global.character_dict_path='+str(dictionary),
               'Train.dataset.data_dir='+str(directory),'Train.dataset.label_file_list='+json.dumps([str(directory/'paddle-train.txt')]),
               'Eval.dataset.data_dir='+str(directory),'Eval.dataset.label_file_list='+json.dumps([str(directory/'paddle-validation.txt')])]
    run([profile['python'],str(repo/'tools/train.py'),'-c',profile['config'],'-o',*overrides],cwd=str(repo))
    checkpoint=output/'best_accuracy'
    if not checkpoint.with_suffix('.pdparams').is_file(): raise ValueError('PaddleOCR produced no best_accuracy checkpoint')
    exported=output/'inference'
    run([profile['python'],str(repo/'tools/export_model.py'),'-c',profile['config'],'-o',
         'Global.pretrained_model='+str(checkpoint),'Global.save_inference_dir='+str(exported),'Global.character_dict_path='+str(dictionary)],cwd=str(repo))
    if not exported.is_dir(): raise ValueError('PaddleOCR model export failed')
    return exported


def train(profile, directory, samples, iterations, run, progress):
    recipes={'tesseract':train_tesseract,'kraken':train_kraken,'paddleocr':train_paddle}
    return recipes[profile['engine']](profile,directory,samples,iterations,run,progress)
