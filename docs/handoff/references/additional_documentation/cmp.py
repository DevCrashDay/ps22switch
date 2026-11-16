import sys, os
from PIL import Image, ImageChops
a, b = sys.argv[1], sys.argv[2]
same = diff = 0
for f in sorted(os.listdir(a)):
    if not f.endswith('.ppm'):
        continue
    pb = os.path.join(b, f)
    if not os.path.exists(pb):
        print(f, 'missing in', b)
        continue
    ia, ib = Image.open(os.path.join(a, f)), Image.open(pb)
    if ia.size != ib.size:
        print(f, 'size', ia.size, ib.size)
        diff += 1
        continue
    d = ImageChops.difference(ia.convert('RGB'), ib.convert('RGB'))
    bbox = d.getbbox()
    if bbox is None:
        same += 1
    else:
        n = sum(1 for px in d.getdata() if px != (0, 0, 0))
        print(f, 'differs:', n, 'pixels, box', bbox, 'max', max(max(px) for px in d.getdata()))
        diff += 1
print('same', same, 'different', diff)
