"""Display-value comparison: no alignment, resizing, gamma or exposure fitting."""
import math
from pathlib import Path
from PIL import Image, ImageChops

METRIC_NAMES = {'normalized_mae', 'normalized_rmse', 'ssim', 'changed_pixel_fraction', 'alpha_mae', 'psnr_db'}


def validate(path, size=None):
    with Image.open(path) as image:
        image.load()
        if size is not None and list(image.size) != list(size):
            raise ValueError('unexpected dimensions: %s, expected %s' % (image.size, size))
        if not image.width or not image.height:
            raise ValueError('empty image')
        return {'width': image.width, 'height': image.height, 'mode': image.mode,
                'flat': all(lo == hi for lo, hi in image.convert('RGB').getextrema())}


def window_ssim(a, b, window=8):
    # Uniform, non-overlapping luminance windows, explicitly versioned in output.
    a, b = a.convert('L'), b.convert('L')
    scores = []
    for y in range(0, a.height, window):
        for x in range(0, a.width, window):
            box = (x, y, min(x + window, a.width), min(y + window, a.height))
            aa, bb = list(a.crop(box).tobytes()), list(b.crop(box).tobytes())
            n = len(aa)
            ma, mb = sum(aa) / n, sum(bb) / n
            va = sum(v * v for v in aa) / n - ma * ma
            vb = sum(v * v for v in bb) / n - mb * mb
            cov = sum(u * v for u, v in zip(aa, bb)) / n - ma * mb
            scores.append(((2 * ma * mb + 6.5025) * (2 * cov + 58.5225)) /
                          ((ma * ma + mb * mb + 6.5025) * (max(0, va) + max(0, vb) + 58.5225)))
    return sum(scores) / len(scores)


def compare(a, b, output, pixel_tolerance=8):
    output = Path(output)
    with Image.open(a) as source:
        aa = source.convert('RGBA')
    with Image.open(b) as source:
        bb = source.convert('RGBA')
    if aa.size != bb.size:
        raise ValueError('image dimension mismatch: %s versus %s' % (aa.size, bb.size))
    alpha_hist = ImageChops.difference(aa.getchannel('A'), bb.getchannel('A')).histogram()
    alpha_mae = sum(i * n for i, n in enumerate(alpha_hist)) / (aa.width * aa.height * 255)
    def flatten(im):
        return Image.alpha_composite(Image.new('RGBA', im.size, (0, 0, 0, 255)), im).convert('RGB')
    aa, bb = flatten(aa), flatten(bb)
    diff = ImageChops.difference(aa, bb)
    histogram = diff.histogram()
    count = aa.width * aa.height * 3
    mae = sum((i % 256) * n for i, n in enumerate(histogram)) / count / 255
    mse = sum((i % 256)**2 * n for i, n in enumerate(histogram)) / count / 255**2
    max_channel = ImageChops.lighter(ImageChops.lighter(diff.getchannel('R'), diff.getchannel('G')), diff.getchannel('B'))
    changed = sum(max_channel.histogram()[pixel_tolerance + 1:]) / (aa.width * aa.height)
    output.mkdir(parents=True, exist_ok=True)
    diff.save(output / 'diff.png')
    heat = Image.merge('RGB', (max_channel.point(lambda v: min(255, v * 4)), max_channel.point(lambda v: min(255, max(0, v - 32) * 4)), Image.new('L', aa.size)))
    heat.save(output / 'heatmap.png')
    return {'width': aa.width, 'height': aa.height, 'normalized_mae': mae,
            'normalized_rmse': math.sqrt(mse), 'psnr_db': -10 * math.log10(mse) if mse else None,
            'psnr_infinite': mse == 0, 'identical': mse == 0 and alpha_mae == 0,
            'ssim': window_ssim(aa, bb), 'ssim_method': 'luminance_uniform_8x8_stride8_population_v1',
            'changed_pixel_fraction': changed, 'pixel_tolerance': pixel_tolerance,
            'alpha_mae': alpha_mae, 'color_policy': 'emitted_values_black_composite',
            'diff': str(output / 'diff.png'), 'heatmap': str(output / 'heatmap.png')}


def gate(metrics, thresholds):
    failures = []
    for metric, limits in thresholds.items():
        if metric not in METRIC_NAMES:
            raise ValueError('unknown threshold metric: ' + metric)
        for operation, limit in limits.items():
            if operation not in ('min', 'max') or not isinstance(limit, (float, int)) or not math.isfinite(limit):
                raise ValueError('threshold requires finite min/max')
            value = metrics.get(metric)
            if metric == 'psnr_db' and metrics.get('psnr_infinite'):
                violated = operation == 'max'
            else:
                violated = value is None or (value < limit if operation == 'min' else value > limit)
            if violated:
                failures.append({'metric': metric, 'operation': operation, 'limit': limit, 'actual': value})
    return {'status': ('fail' if failures else 'pass') if thresholds else 'uncalibrated', 'failures': failures, 'thresholds': thresholds}
