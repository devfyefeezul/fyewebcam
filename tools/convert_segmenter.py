"""Builds windows/models/selfie_segmenter.onnx from Google's MediaPipe selfie segmenter.

The model ships as TFLite with one MediaPipe custom op (Convolution2DTransposeBias), which tf2onnx
can't convert. This converts the rest and swaps that op for a standard ConvTranspose.

    pip install tensorflow-cpu tf2onnx onnx
    python3 tools/convert_segmenter.py
"""

import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

import numpy as np
import onnx
from onnx import helper, numpy_helper

URL = ("https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter/"
       "float16/latest/selfie_segmenter.tflite")
OUT = Path(__file__).resolve().parent.parent / "windows/models/selfie_segmenter.onnx"


def replace_transpose_conv(model):
    g = model.graph
    inits = {i.name: i for i in g.initializer}
    node = next(n for n in g.node if n.op_type == "TFL_Convolution2DTransposeBias")
    # tf2onnx feeds it NHWC through a transpose from the NCHW graph; ConvTranspose wants NCHW.
    pre = next(n for n in g.node if node.input[0] in n.output)
    assert pre.op_type == "Transpose" and list(pre.attribute[0].ints) == [0, 2, 3, 1]
    w = numpy_helper.to_array(inits[node.input[1]])  # TFLite: [out, kh, kw, in]
    b = numpy_helper.to_array(inits[node.input[2]])
    g.initializer.append(numpy_helper.from_array(
        np.ascontiguousarray(w.transpose(3, 0, 1, 2)).astype(np.float32), "convT_w"))  # [in, out, kh, kw]
    g.initializer.append(numpy_helper.from_array(b.astype(np.float32), "convT_b"))
    kh, kw = int(w.shape[1]), int(w.shape[2])
    idx = list(g.node).index(node)
    g.node.remove(node)
    g.node.insert(idx, helper.make_node("Transpose", ["convT_nchw"], [node.output[0]], perm=[0, 2, 3, 1]))
    g.node.insert(idx, helper.make_node("ConvTranspose", [pre.input[0], "convT_w", "convT_b"], ["convT_nchw"],
                                        kernel_shape=[kh, kw], strides=[2, 2]))
    if not any(pre.output[0] in n.input for n in g.node):
        g.node.remove(pre)
    used = {i for n in g.node for i in n.input}
    keep = [i for i in g.initializer if i.name in used]
    del g.initializer[:]
    g.initializer.extend(keep)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tflite = Path(tmp) / "selfie_segmenter.tflite"
        raw = Path(tmp) / "raw.onnx"
        urllib.request.urlretrieve(URL, tflite)
        subprocess.run([sys.executable, "-m", "tf2onnx.convert", "--tflite", str(tflite), "--output", str(raw),
                        "--opset", "14"], check=True)
        model = onnx.load(raw)
    replace_transpose_conv(model)
    onnx.checker.check_model(model)
    onnx.save(model, OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
