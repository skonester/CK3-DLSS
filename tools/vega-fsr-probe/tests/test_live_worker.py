import ctypes as C
from pathlib import Path
import sys
import unittest
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from live_worker import Header, PLANE, OUTPUT, NativeConverter, convert_inputs, convert_output, choose_gpu


class LiveBridgeTests(unittest.TestCase):
    def test_packet_matches_native_offsets(self):
        self.assertEqual(C.sizeof(Header), 1024)
        self.assertEqual(Header.frameId.offset, 64)
        self.assertEqual(Header.gpuName.offset, 104)
        self.assertEqual(Header.bgra.offset, 744)

    def test_gpu_selection_never_falls_back(self):
        devices = [dict(vendor_id=0x10de, device_type='discrete', name='RTX'),
                   dict(vendor_id=0x1002, device_type='integrated', name='Vega')]
        self.assertEqual(choose_gpu(devices, 'Integrated')['name'], 'Vega')
        self.assertEqual(choose_gpu(devices, 'Dedicated')['name'], 'RTX')
        with self.assertRaisesRegex(RuntimeError, 'No fallback'):
            choose_gpu(devices[:1], 'Integrated')

    def test_bgra_linear_conversion_and_pixel_motion_scale(self):
        color = np.zeros((720, 1280, 4), np.uint8)
        color[:] = [64, 128, 255, 255]
        depth = np.full((720, 1280), .5, np.float32)
        motion = np.full((720, 1280, 2), 3, np.float16)
        packed = convert_inputs(color.tobytes()+depth.tobytes()+motion.tobytes(), True)
        self.assertEqual(len(packed), PLANE*4)
        rgba = np.frombuffer(packed[:PLANE*2], np.float16).reshape(720,1280,4)
        self.assertEqual(rgba[0,0,0], 1)
        self.assertAlmostEqual(float(rgba[0,0,1]), .21586, places=3)
        self.assertTrue(np.all(np.frombuffer(packed[PLANE*3:], np.float16) == 2))
        self.assertEqual(packed[PLANE*2:PLANE*3], depth.tobytes())

    def test_nonfinite_guides_fail(self):
        data = bytearray(PLANE*3)
        data[PLANE:PLANE+4] = np.float32(np.nan).tobytes()
        with self.assertRaisesRegex(RuntimeError, 'Nonfinite'):
            convert_inputs(data, False)

    def test_output_is_srgb_bgra_and_finite(self):
        image = np.ones((1080,1920,4), np.float16)
        image[:,:,:3] = [1,.5,0]
        result = np.frombuffer(convert_output(image.tobytes(), True), np.uint8).reshape(1080,1920,4)
        self.assertEqual(result[0,0].tolist(), [0,188,255,255])
        image[0,0,0] = np.nan
        with self.assertRaisesRegex(RuntimeError, 'Nonfinite'):
            convert_output(image.tobytes(), False)


@unittest.skipUnless((Path(__file__).resolve().parents[3]/'build/ck3-fsr-convert.dll').exists(),
                     'Build the native measurement converter first')
class NativeConversionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.converter=NativeConverter(Path(__file__).resolve().parents[3]/'build/ck3-fsr-convert.dll')

    def test_native_input_matches_float_reference_for_both_channel_orders(self):
        rng=np.random.default_rng(17)
        color=rng.integers(0,256,(720,1280,4),dtype=np.uint8)
        depth=np.full((720,1280),.5,np.float32)
        vectors=rng.uniform(-300,300,(720,1280,2)).astype(np.float16)
        raw=color.tobytes()+depth.tobytes()+vectors.tobytes()
        src=C.create_string_buffer(raw); dst=C.create_string_buffer(PLANE*4)
        for bgra in [False,True]:
            rgb=color[:,:,:3].astype(np.float32)/255
            if bgra: rgb=rgb[:,:,::-1]
            rgba=np.ones((720,1280,4),np.float16)
            rgba[:,:,:3]=np.where(rgb<=.04045,rgb/12.92,((rgb+.055)/1.055)**2.4)
            expected=rgba.tobytes()+depth.tobytes()+(vectors.astype(np.float32)*(2/3)).astype(np.float16).tobytes()
            self.converter.input(src,dst,bgra)
            self.assertEqual(dst.raw,expected)

    def test_native_output_covers_every_finite_half_pattern(self):
        patterns=np.arange(65536,dtype=np.uint16)
        patterns[(patterns&0x7c00)==0x7c00]=0
        image=np.zeros((1080,1920,4),np.uint16)
        image[:,:,0]=np.resize(patterns,(1080,1920))
        image[:,:,1]=np.resize(np.roll(patterns,100),(1080,1920))
        image[:,:,2]=np.resize(np.roll(patterns,200),(1080,1920))
        src=C.create_string_buffer(image.tobytes()); dst=C.create_string_buffer(OUTPUT)
        rgb=np.clip(image.view(np.float16).astype(np.float32)[:,:,:3],0,1)
        rgb=np.where(rgb<=.0031308,rgb*12.92,1.055*rgb**(1/2.4)-.055)
        for bgra in [False,True]:
            expected=np.full((1080,1920,4),255,np.uint8)
            expected[:,:,:3]=np.round(np.clip(rgb[:,:,::-1] if bgra else rgb,0,1)*255).astype(np.uint8)
            self.converter.output(src,dst,bgra)
            self.assertEqual(dst.raw,expected.tobytes())

    def test_native_conversion_rejects_nonfinite_data(self):
        raw=bytearray(PLANE*3); target=C.create_string_buffer(PLANE*4)
        raw[PLANE:PLANE+4]=np.float32(np.inf).tobytes()
        with self.assertRaisesRegex(RuntimeError,'Nonfinite'):
            self.converter.input(C.create_string_buffer(bytes(raw)),target,False)
        raw[PLANE:PLANE+4]=bytes(4)
        raw[PLANE*2:PLANE*2+2]=np.float16(np.nan).tobytes()
        with self.assertRaisesRegex(RuntimeError,'Nonfinite'):
            self.converter.input(C.create_string_buffer(bytes(raw)),target,True)
        out=np.zeros((1080,1920,4),np.float16); out[-1,-1,2]=np.nan
        with self.assertRaisesRegex(RuntimeError,'Nonfinite'):
            self.converter.output(C.create_string_buffer(out.tobytes()),C.create_string_buffer(OUTPUT),True)


if __name__ == '__main__':
    unittest.main()
