"""Opt-in OCR adapters. Importing this module loads no inference frameworks."""
import base64
import copy
import html
from html.parser import HTMLParser
import io
import json
import math
import os
from pathlib import Path
import re
import urllib.request
from urllib.parse import urlsplit, urlunsplit

MAX_RESPONSE = 8 * 1024 * 1024


def model_catalog():
    return json.loads(Path(__file__).with_name('optional-models.example.json').read_text(encoding='utf-8'))


def model_spec(identifier):
    return copy.deepcopy(next(entry for entry in model_catalog() if entry['id'] == identifier))


def endpoint_url(endpoint):
    url = urlsplit(endpoint)
    if url.scheme not in ('http', 'https') or not url.hostname or url.username or url.password or url.query or url.fragment:
        raise ValueError('Use an HTTP(S) upstream URL without credentials, query or fragment')
    path = url.path.rstrip('/')
    if not path.endswith('/chat/completions'):
        path += '/chat/completions' if path.endswith('/v1') else '/v1/chat/completions'
    return urlunsplit((url.scheme, url.netloc, path, '', ''))


def evidence(backend, width, height, task, page_number):
    return dict(engine_id=backend.engine_id, model_version='', text=[], formulas=[], tables=[], figures=[],
                markdown='', structured_output={}, provenance=dict(model=backend.model, task=task,
                language='auto', page_number=page_number, width=width, height=height))


def pixel_box(bounds, width, height, scale):
    if not isinstance(bounds, (list, tuple)) or len(bounds) != 4 or any(
            isinstance(n, bool) or not isinstance(n, (float, int)) or not math.isfinite(n) for n in bounds):
        raise ValueError('Invalid model coordinates')
    left, top, right, bottom = bounds
    if not 0 <= left < right <= scale or not 0 <= top < bottom <= scale:
        raise ValueError('Model coordinates lie outside the page')
    x1, y1 = math.floor(left * width / scale), math.floor(top * height / scale)
    x2, y2 = math.ceil(right * width / scale), math.ceil(bottom * height / scale)
    return [x1, y1, x2 - x1, y2 - y1]


class PlainText(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []

    def handle_data(self, data):
        self.parts.append(data)

    def handle_starttag(self, tag, _attrs):
        if tag == 'br':
            self.parts.append('\n')

    def handle_endtag(self, tag):
        if tag in ('td', 'th'):
            self.parts.append('\t')
        elif tag in ('tr', 'p', 'div'):
            self.parts.append('\n')


def plain_text(value):
    parser = PlainText()
    parser.feed(value)
    return ''.join(parser.parts).strip()


def latex_text(value):
    value = value.strip()
    for left, right in (('$$', '$$'), ('\\[', '\\]'), ('\\(', '\\)'), ('$', '$')):
        if value.startswith(left) and value.endswith(right):
            return value[len(left):-len(right)].strip()
    return value


def add_block(result, label, content, box=None, html_content=False, confidence=-1):
    if not isinstance(content, str) or len(content) > 65536:
        raise ValueError('Invalid or oversized OCR block')
    count = sum(len(result[k]) for k in ('text', 'formulas', 'tables', 'figures'))
    if count >= 10000:
        raise ValueError('Too many OCR blocks')
    if not isinstance(label, str) or not label or len(label) > 128:
        raise ValueError('Invalid OCR block label')
    region = dict(id='block-' + str(count), parent_id='', order=count, confidence=confidence)
    if box is not None:
        region['box'] = box
    kind = label.lower().rstrip('+').replace('_', '-')
    text = plain_text(content) if html_content else content.strip()
    if kind in ('formula', 'equation', 'display-formula', 'equation-block', 'math'):
        region.update(latex=latex_text(text), display=True)
        result['formulas'].append(region)
    elif kind == 'table':
        region.update(text=text, markdown='' if html_content else content,
                      html=content if html_content else '', structure={})
        result['tables'].append(region)
    elif kind in ('image', 'figure', 'chart', 'header-image', 'footer-image'):
        region.update(caption=text, type=label)
        result['figures'].append(region)
    elif text:
        region.update(text=text, type=label, granularity='block', block=0, paragraph=0, line=0)
        result['text'].append(region)


def lighton_result(backend, text, width, height, task, page_number):
    result = evidence(backend, width, height, task, page_number)
    if task == 'formula':
        add_block(result, 'formula', text)
        result['structured_output'] = {'raw_text': text}
        return result
    marker = re.compile(r'^!\[([A-Za-z_+.-]+)\]\(\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?)\s*\)[ \t]*$', re.M)
    matches = list(marker.finditer(text))
    if not matches:
        if task == 'layout':
            raise ValueError('LightOnOCR returned no grounding coordinates')
        result['markdown'] = text
        return result
    if len(matches) > 10000:
        raise ValueError('Too many grounding blocks')
    markdown = [text[:matches[0].start()].strip()]
    for index, match in enumerate(matches):
        content = text[match.end():matches[index + 1].start() if index + 1 < len(matches) else len(text)].strip()
        bounds = pixel_box([float(n) for n in match.groups()[1:]], width, height, 1000)
        add_block(result, match[1], content, bounds, html_content=content.lstrip().startswith('<table'))
        markdown.append(content)
    result['markdown'] = '\n\n'.join(part for part in markdown if part)
    result['structured_output'] = {'grounding': text}
    return result


class ChatBackend:
    remote_inference = True
    def __init__(self, identifier, endpoint, model=None):
        spec = model_spec(identifier)
        self.engine_id = spec['provider']
        self.profile = spec['profile']
        self.model = model or self.profile['model']
        self.profile['model'] = self.model
        self.tasks = tuple('ocr' if task == 'text' else task for task in self.profile['tasks'])
        self.language = 'auto'
        self.endpoint = endpoint_url(endpoint)
        self.token = os.environ.get('YAGF_UPSTREAM_TOKEN', '')
        self.lighton = identifier == 'lightonocr'

    def recognize_evidence(self, png, width, height, language, task, page_number):
        if language != 'auto' or task not in self.tasks:
            raise ValueError('Unsupported model language or task')
        prompt = ('' if task == 'formula' else 'grounding') if self.lighton else {
            'ocr': 'Text Recognition:', 'formula': 'Formula Recognition:', 'table': 'Table Recognition:'}[task]
        content = [dict(type='image_url', image_url=dict(url='data:image/png;base64,' + base64.b64encode(png).decode()))]
        if prompt:
            content.append(dict(type='text', text=prompt))
        payload = dict(model=self.model, messages=[dict(role='user', content=content)], max_tokens=8192,
                       temperature=0.2 if self.lighton else 0)
        if self.lighton:
            payload['chat_template_kwargs'] = {'enable_thinking': False}
        headers = {'Content-Type': 'application/json'}
        if self.token:
            headers['Authorization'] = 'Bearer ' + self.token
        request = urllib.request.Request(self.endpoint, json.dumps(payload).encode(), headers)
        # Do not forward document images or bearer tokens through redirects.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *_args, **_kwargs):
                return None
        with urllib.request.build_opener(NoRedirect).open(request, timeout=150) as response:
            data = response.read(MAX_RESPONSE + 1)
        if len(data) > MAX_RESPONSE:
            raise ValueError('Model response exceeds the size limit')
        root = json.loads(data)
        choices = root.get('choices') if isinstance(root, dict) else None
        if not isinstance(choices, list) or len(choices) != 1 or not isinstance(choices[0], dict):
            raise ValueError('Invalid chat response')
        choice = choices[0]
        if choice.get('finish_reason') != 'stop':
            raise ValueError('Incomplete or truncated model response')
        message = choice.get('message')
        text = message.get('content') if isinstance(message, dict) else None
        if not isinstance(text, str) or not text.strip():
            raise ValueError('Empty or invalid model output')
        if self.lighton:
            return lighton_result(self, text, width, height, task, page_number)
        result = evidence(self, width, height, task, page_number)
        result['markdown'] = text if task == 'ocr' else ''
        if task != 'ocr':
            add_block(result, task, text, html_content=task == 'table' and text.lstrip().startswith('<'))
        result['structured_output'] = {'raw_text': text}
        return result


def page_image(png, width, height):
    from PIL import Image
    with Image.open(io.BytesIO(png)) as original:
        if original.format != 'PNG' or original.size != (width, height):
            raise ValueError('Incompatible page image')
        if original.mode in ('RGBA', 'LA') or 'transparency' in original.info:
            rgba = original.convert('RGBA')
            image = Image.new('RGB', rgba.size, 'white')
            image.paste(rgba, mask=rgba.getchannel('A'))
            return image
        return original.convert('RGB')


class FormulaNetBackend:
    def __init__(self, device='cpu'):
        from doctr.models import formula_predictor
        self.profile = model_spec('doctr-formulanet')['profile']
        self.model = self.profile['model']
        self.engine_id, self.language, self.tasks = 'doctr', 'auto', ('formula',)
        self.predictor = formula_predictor('formulanet', pretrained=True).to('cuda' if device == 'gpu' else device)

    def recognize_evidence(self, png, width, height, language, task, page_number):
        if language != 'auto' or task != 'formula':
            raise ValueError('docTR FormulaNet accepts formula crops with automatic language only')
        import numpy as np
        predictions = self.predictor([np.asarray(page_image(png, width, height))])
        if len(predictions) != 1 or not isinstance(predictions[0], (tuple, list)) or len(predictions[0]) != 2:
            raise ValueError('Invalid docTR FormulaNet result')
        text, confidence = predictions[0]
        if not isinstance(text, str) or not text.strip() or not math.isfinite(float(confidence)) or not 0 <= float(confidence) <= 1:
            raise ValueError('Invalid formula transcription or confidence')
        result = evidence(self, width, height, task, page_number)
        add_block(result, 'formula', text, confidence=float(confidence) * 100)
        return result


class MinerUBackend:
    remote_inference = True
    def __init__(self, endpoint, model=None):
        from mineru_vl_utils import MinerUClient
        self.profile = model_spec('mineru')['profile']
        self.model = model or self.profile['model']
        self.profile['model'] = self.model
        self.engine_id, self.language, self.tasks = 'mineru', 'auto', ('ocr', 'layout', 'formula', 'table')
        url = endpoint_url(endpoint).removesuffix('/chat/completions')
        token = os.environ.get('YAGF_UPSTREAM_TOKEN', '')
        self.client = MinerUClient(backend='http-client', server_url=url, model_name=self.model,
                                  http_timeout=150,
                                  server_headers={'Authorization': 'Bearer ' + token} if token else None)

    def recognize_evidence(self, png, width, height, language, task, page_number):
        if language != 'auto' or task not in self.tasks:
            raise ValueError('Unsupported MinerU language or task')
        from mineru_vl_utils.post_process import json2md
        blocks = self.client.two_step_extract(page_image(png, width, height))
        if not isinstance(blocks, list) or len(blocks) > 10000:
            raise ValueError('Invalid MinerU block result')
        result = evidence(self, width, height, task, page_number)
        for block in blocks:
            if not isinstance(block, dict):
                raise ValueError('Invalid MinerU block')
            label, content = block.get('type', 'text'), block.get('content') or ''
            bounds = pixel_box(block['bbox'], width, height, 1) if block.get('bbox') is not None else None
            add_block(result, label, content, bounds, html_content=label == 'table' and content.lstrip().startswith('<'))
        if task == 'formula' and not result['formulas']:
            raise ValueError('MinerU found no formula in this crop')
        result['markdown'] = json2md(blocks)
        result['structured_output'] = {'blocks': blocks}
        return result


class ChandraBlocks(HTMLParser):
    """Read actual raw boxes; the SDK substitutes input extents for missing boxes."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.blocks, self.depth, self.current = [], 0, None

    def handle_starttag(self, tag, attrs):
        if tag == 'div':
            if self.depth == 0:
                values = dict(attrs)
                self.current = dict(label=values.get('data-label') or 'text', bbox=values.get('data-bbox'), content='')
            self.depth += 1
            if self.depth == 1:
                return
        if self.current:
            self.current['content'] += self.get_starttag_text()

    def handle_endtag(self, tag):
        if tag == 'div':
            self.depth = max(0, self.depth - 1)
            if self.depth == 0 and self.current:
                self.blocks.append(self.current)
                self.current = None
                return
        if self.current:
            self.current['content'] += '</' + tag + '>'

    def handle_data(self, data):
        if self.current:
            self.current['content'] += html.escape(data)


class ChandraBackend:
    remote_inference = True
    def __init__(self, endpoint, model=None):
        from chandra import InferenceManager, BatchInputItem
        self.profile = model_spec('chandra')['profile']
        self.model = model or self.profile['model']
        self.profile['model'] = self.model
        self.engine_id, self.language, self.tasks = 'chandra', 'auto', ('ocr', 'layout', 'formula', 'table')
        self.manager, self.input_type = InferenceManager(method='vllm'), BatchInputItem
        self.api_base = endpoint_url(endpoint).removesuffix('/chat/completions')
        self.token = os.environ.get('YAGF_UPSTREAM_TOKEN', '')

    def recognize_evidence(self, png, width, height, language, task, page_number):
        if language != 'auto' or task not in self.tasks:
            raise ValueError('Unsupported Chandra language or task')
        pages = self.manager.generate([self.input_type(image=page_image(png, width, height), prompt_type='ocr_layout')],
            max_output_tokens=8192, bbox_scale=1000, vllm_api_base=self.api_base,
            vllm_api_key=self.token, vllm_model_name=self.model, include_headers_footers=True)
        if len(pages) != 1 or pages[0].error or not isinstance(pages[0].raw, str):
            raise ValueError('Incomplete or invalid Chandra output')
        page = pages[0]
        result = evidence(self, width, height, task, page_number)
        parser = ChandraBlocks()
        parser.feed(page.raw)
        if parser.depth or parser.current:
            raise ValueError('Incomplete Chandra layout output')
        for block in parser.blocks:
            raw = block['bbox']
            bounds = None
            if raw:
                coords = re.fullmatch(r'\s*\[?\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*\]?\s*', raw)
                if coords is None:
                    raise ValueError('Invalid Chandra bounding box')
                bounds = pixel_box([float(n) for n in coords.groups()], width, height, 1000)
            add_block(result, block['label'], block['content'], bounds, html_content=True)
        if task == 'formula' and not result['formulas']:
            raise ValueError('Chandra found no formula in this crop')
        result['markdown'] = page.markdown
        result['structured_output'] = {'html': page.raw}
        return result
