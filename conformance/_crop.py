from PIL import Image

br = Image.open('conformance/out/bs_badges.browser.default.png').convert('RGB')
af = Image.open('conformance/out/bs_badges.affineui.default.ppm').convert('RGB')

# Crop the Heading + Notifications region (y ~80..175, x 0..360)
box = (0, 80, 360, 178)
bc = br.crop(box)
ac = af.crop(box)

w = box[2]-box[0]
h = box[3]-box[1]
# Stack: Chrome on top, label gap, ours below, scaled 2x for clarity
scale = 2
canvas = Image.new('RGB', (w*scale, h*scale*2 + 12), (255,255,255))
canvas.paste(bc.resize((w*scale, h*scale), Image.NEAREST), (0,0))
canvas.paste(ac.resize((w*scale, h*scale), Image.NEAREST), (0, h*scale+12))
canvas.save('conformance/out/_cmp_badges.png')
print('saved _cmp_badges.png  (top=chrome, bottom=affineui, 2x)')
