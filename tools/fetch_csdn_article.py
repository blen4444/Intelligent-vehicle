# -*- coding: utf-8 -*-
"""抓取 CSDN 博客正文，转成 Markdown 并把配图下载到本地（离线可读）。

依赖: pip install beautifulsoup4 markdownify
用法:
    python tools/fetch_csdn_article.py <文章URL>
    python tools/fetch_csdn_article.py <文章URL> --html 已保存的.html
    python tools/fetch_csdn_article.py <文章URL> --out-dir 目标目录

说明: CSDN 会概率性返回 HTTP 521(JS 挑战页)，本脚本用多组 UA / 域名重试绕过。
      文件默认输出到脚本上一级目录(项目根)，配图放在 assets/csdn_<文章ID>/。
"""
import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys

try:
    from bs4 import BeautifulSoup, NavigableString
    from markdownify import markdownify as to_md
except ImportError:
    sys.exit('缺少依赖，请先执行: pip install beautifulsoup4 markdownify')

UA_IPHONE = ('Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) '
             'AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.0 '
             'Mobile/15E148 Safari/604.1')
UA_CHROME = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) '
             'AppleWebKit/537.36 (KHTML, like Gecko) '
             'Chrome/120.0.0.0 Safari/537.36')


def log(*a):
    print(*a, flush=True)


def curl_bin():
    p = shutil.which('curl')
    if p:
        return p
    for c in [r'C:\Program Files\Git\mingw64\bin\curl.exe',
              r'C:\Program Files\Git\usr\bin\curl.exe',
              r'C:\Users\Administrator\.workbuddy\binaries\PortableGit\usr\bin\curl.exe']:
        if os.path.exists(c):
            return c
    return None


def fetch_html(url):
    """返回 (html文本, 实际使用的URL)；失败返回 (None, None)。"""
    art_id = re.search(r'/details/(\d+)', url)
    if not art_id:
        sys.exit('URL 里找不到 /details/<数字ID>')
    art_id = art_id.group(1)

    variants = [
        (UA_IPHONE, url),
        (UA_IPHONE, 'https://m.blog.csdn.net/%s/article/details/%s'
         % (re.search(r'blog\.csdn\.net/([^/]+)/', url).group(1), art_id)),
        (UA_IPHONE, 'https://blog.csdn.net/%s/article/details/%s?spm=1001.2014.3001.5501'
         % (re.search(r'blog\.csdn\.net/([^/]+)/', url).group(1), art_id)),
        (UA_CHROME, 'https://blog.csdn.net/%s/article/details/%s?type=blog'
         % (re.search(r'blog\.csdn\.net/([^/]+)/', url).group(1), art_id)),
        (UA_CHROME, url),
        (UA_IPHONE, url),   # 再试一次，521 是概率性的
    ]
    curl = curl_bin()
    if not curl:
        sys.exit('找不到 curl，请先手动保存 HTML 再用 --html 参数')
    for i, (ua, u) in enumerate(variants, 1):
        try:
            r = subprocess.run(
                [curl, '-sL', '--max-time', '25', '-A', ua,
                 '-H', 'Referer: https://blog.csdn.net/',
                 '-H', 'Accept-Language: zh-CN,zh;q=0.9', u],
                capture_output=True, timeout=40)
            data = r.stdout
        except Exception as e:
            log('  try %d failed: %s' % (i, e))
            continue
        ok = b'content_views' in data and len(data) > 50000
        log('  try %d: %d bytes, content_views=%s' % (i, len(data), ok))
        if ok:
            return data.decode('utf-8', errors='replace'), u
    return None, None


def safe_name(s):
    s = s.replace('原创', '').strip()
    return re.sub(r'[\\/:*?"<>|]', '_', s).strip(' .')


def clean_text(t):
    t = t.replace('\u200b', '').replace('\u200c', '').replace('\u200d', '')
    return t.replace('\xa0', ' ').replace('\r\n', '\n')


def parse_meta(soup, url):
    title_el = soup.find('h1', class_='article_title')
    title = safe_name(title_el.get_text(' ', strip=True) if title_el
                      else 'CSDN_' + re.search(r'/details/(\d+)', url).group(1))
    box = soup.find(class_='creat-time-box')
    raw = clean_text(box.get_text(' ', strip=True)) if box else ''
    modified = re.search(r'已于\s*([\d-]+\s[\d:]+)\s*修改', raw)
    dates = re.findall(r'\d{4}-\d{2}-\d{2}\s\d{2}:\d{2}:\d{2}', raw)
    published = ''
    for d in dates:
        if not modified or d != modified.group(1):
            published = d
            break
    reads = re.search(r'阅读量\s*([\d.]+k?)', raw)
    favs = re.search(r'收藏\s*(\d+)', raw)
    likes = re.search(r'(\d+)\s*赞', raw)
    author_m = re.search(r'blog\.csdn\.net/([^/]+)/', url)
    return {
        'title': title,
        'author': author_m.group(1) if author_m else '',
        'published': published,
        'modified': modified.group(1) if modified else '',
        'reads': reads.group(1) if reads else '',
        'favs': favs.group(1) if favs else '',
        'likes': likes.group(1) if likes else '',
        'raw': raw,
    }


def download_images(urls, out_dir, assets_rel):
    """返回 {原URL: 本地相对路径或原URL}；失败保留原外链。"""
    curl = curl_bin()
    abs_dir = os.path.join(out_dir, assets_rel.replace('/', os.sep))
    os.makedirs(abs_dir, exist_ok=True)
    mapping = {}
    for i, u in enumerate(urls):
        name = u.rsplit('/', 1)[-1].split('?')[0]
        if not re.search(r'\.(png|jpe?g|gif|webp|bmp)$', name, re.I):
            name = 'img_%02d.png' % i
        if name in [v.rsplit('/', 1)[-1] for v in mapping.values()]:
            name = '%02d_%s' % (i, name)
        dst = os.path.join(abs_dir, name)
        rel = assets_rel + '/' + name
        got = False
        if os.path.exists(dst) and os.path.getsize(dst) > 1000:
            got = True                      # 重跑时复用已下载的图片
        elif curl and u.startswith('http'):
            try:
                subprocess.run(
                    [curl, '-sL', '--max-time', '25', '-A', UA_CHROME,
                     '-H', 'Referer: https://blog.csdn.net/', u, '-o', dst],
                    capture_output=True, timeout=35)
                with open(dst, 'rb') as f:
                    head = f.read(8)
                got = os.path.getsize(dst) > 1000 and (head.startswith(b'\x89PNG')
                                                       or head.startswith(b'\xff\xd8')
                                                       or head[:4] in (b'GIF8', b'RIFF'))
                if not got:
                    os.remove(dst)
            except Exception as e:
                log('  image failed: %s (%s)' % (u, e))
        mapping[u] = rel if got else u
        log('  image %2d %s -> %s' % (i + 1, 'OK  ' if got else 'LINK',
                                      rel if got else u))
    return mapping


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('url', help='CSDN 文章 URL')
    ap.add_argument('--html', help='复用本地已保存的 HTML（跳过网络抓取）')
    ap.add_argument('--out-dir', help='输出目录，默认脚本上一级目录')
    args = ap.parse_args()

    url = args.url.strip()
    art_id = re.search(r'/details/(\d+)', url)
    if not art_id:
        sys.exit('URL 里找不到文章 ID')
    art_id = art_id.group(1)
    clean_url = re.sub(r'\?.*$', '', url)

    out_dir = args.out_dir or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.makedirs(out_dir, exist_ok=True)

    if args.html:
        html = open(args.html, encoding='utf-8', errors='replace').read()
        log('HTML from:', args.html, len(html), 'chars')
    else:
        log('Fetching:', clean_url)
        html, used = fetch_html(url)
        if not html:
            sys.exit('抓取失败：所有 UA/域名组合都返回 521，可稍后重试或用 --html')
        log('Got HTML via:', used)
        import tempfile
        tmp = os.path.join(tempfile.gettempdir(), 'csdn_%s_source.html' % art_id)
        open(tmp, 'w', encoding='utf-8').write(html)
        log('Raw HTML saved:', tmp)

    soup = BeautifulSoup(html, 'html.parser')
    meta = parse_meta(soup, url)
    content = soup.find('div', id='content_views')
    if content is None:
        sys.exit('HTML 里没有 div#content_views，可能是挑战页')
    for e in content.find_all(['link', 'script', 'style']):
        e.decompose()

    # 收集正文图片（统计数在替换占位符之前取）
    n_p = len(content.find_all('p'))
    n_pre = len(content.find_all('pre'))
    n_img = len(content.find_all('img'))
    img_urls = []
    for im in content.find_all('img'):
        u = im.get('src') or im.get('data-src') or ''
        if u.startswith('http') and u not in img_urls:
            img_urls.append(u)
    assets_rel = 'assets/csdn_' + art_id
    log('Images found:', len(img_urls))
    img_map = download_images(img_urls, out_dir, assets_rel) if img_urls else {}
    n_local = sum(1 for v in img_map.values() if not v.startswith('http'))

    blocks = []
    for node in content.find_all(recursive=False):
        if node.name == 'pre':
            code = node.find('code')
            lang = ''
            if code:
                for cls in (code.get('class') or []):
                    if cls.startswith('language-'):
                        lang = cls[len('language-'):]
            txt = clean_text(code.get_text() if code else node.get_text())
            lines = [ln.rstrip() for ln in txt.split('\n')]
            while lines and not lines[0].strip():
                lines.pop(0)
            while lines and not lines[-1].strip():
                lines.pop()
            compact = []
            for ln in lines:
                if ln == '' and compact and compact[-1] == '':
                    continue
                compact.append(ln)
            blocks.append('```' + lang + '\n' + '\n'.join(compact) + '\n```')
            continue

        # 图片用占位符替换，保证顺序不变且每张图独立成行
        holders = []
        for idx, im in enumerate(node.find_all('img')):
            src = im.get('src') or im.get('data-src') or ''
            token = 'ZZIMGTOKEN%dZZ' % idx
            holders.append((token, img_map.get(src, src)))
            im.replace_with(NavigableString(token))
        text = clean_text(to_md(str(node), heading_style='ATX'))
        for token, path in holders:
            text = text.replace(token, '\n\n![](' + path + ')\n\n')
        text = re.sub(r'\n{3,}', '\n\n', text).strip()
        if text:
            blocks.append(text)

    body = re.sub(r'\n{3,}', '\n\n', '\n\n'.join(blocks)).strip()
    now = datetime.datetime.now().strftime('%Y-%m-%d %H:%M')

    header = [
        '# ' + meta['title'], '',
        '> **原文**：[' + clean_url + '](' + clean_url + ')',
        '> **作者**：' + meta['author'] + '（原创）',
    ]
    date_parts = []
    if meta['published']:
        date_parts.append('**发布时间**：' + meta['published'])
    if meta['modified']:
        date_parts.append('**最后修改**：' + meta['modified'])
    if date_parts:
        header.append('> ' + '　|　'.join(date_parts))
    stats = '　|　'.join(x for x in [
        ('阅读 ' + meta['reads']) if meta['reads'] else '',
        ('收藏 ' + meta['favs']) if meta['favs'] else '',
        ('点赞 ' + meta['likes']) if meta['likes'] else '',
    ] if x)
    if stats:
        header.append('> **数据**：' + stats)
    header += [
        '> **抓取保存**：' + now + '　|　配图本地目录 `' + assets_rel + '/`（%d/%d 张已下载）'
        % (n_local, len(img_urls)),
        '', '---', '',
    ]

    footer = ['']
    if img_urls:
        footer += ['---', '', '## 附：配图原始地址', '']
        footer += ['%d. %s' % (i, u) for i, u in enumerate(img_urls, 1)]
        footer.append('')
    footer.append('> 本文为原文正文的完整转录（含代码块与配图），文字与代码未做改写；'
                  '仅补了上述元信息、本地图片路径，并把并排图片拆成独立行以便阅读。')

    doc = '\n'.join(header) + '\n' + body + '\n' + '\n'.join(footer) + '\n'
    assert 'ZZIMGTOKEN' not in doc, '有图片占位符未替换'
    out_md = os.path.join(out_dir, safe_name(meta['title']) + '.md')
    open(out_md, 'w', encoding='utf-8', newline='\n').write(doc)

    log('---')
    log('OUT:', out_md)
    log('size: %d bytes / %d chars / %d lines'
        % (os.path.getsize(out_md), len(doc), len(doc.split('\n'))))
    log('source: %d p, %d pre, %d img; md: %d code fences, %d images'
        % (n_p, n_pre, n_img, doc.count('```'), doc.count('![')))
    log('residual html: %d, entities: %d, replacement chars: %d'
        % (len(re.findall(r'</?(?:p|div|span|pre|code)\b', body)),
           len(re.findall(r'&#?\w+;', body)), doc.count('\ufffd')))


if __name__ == '__main__':
    main()
