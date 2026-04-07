from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


ROOT = Path(__file__).resolve().parents[1]
MODEL_DIR = ROOT / "model"
VOCAB = json.loads((MODEL_DIR / "vocab.json").read_text())["tokens"]
TRANSITIONS = json.loads((MODEL_DIR / "transitions.json").read_text())


def build_transition_matrix() -> np.ndarray:
    size = len(VOCAB)
    matrix = np.zeros((size, size), dtype=np.float32)
    for index, token in enumerate(VOCAB):
        next_token = TRANSITIONS[token]
        next_index = VOCAB.index(next_token)
        matrix[index, next_index] = 1.0
    return matrix


def main() -> None:
    matrix = build_transition_matrix()
    input_tensor = helper.make_tensor_value_info("input_token", TensorProto.INT64, [1])
    output_tensor = helper.make_tensor_value_info("next_token", TensorProto.INT64, [1])

    transition_initializer = numpy_helper.from_array(matrix, name="transition_matrix")
    gather_node = helper.make_node("Gather", ["transition_matrix", "input_token"], ["logits"], axis=0)
    argmax_node = helper.make_node("ArgMax", ["logits"], ["next_token"], axis=1, keepdims=0)

    graph = helper.make_graph(
        [gather_node, argmax_node],
        "next_word_bigram",
        [input_tensor],
        [output_tensor],
        [transition_initializer],
    )
    model = helper.make_model(graph, producer_name="edgeai-toy-generator")
    model.opset_import[0].version = 13
    onnx.checker.check_model(model)
    onnx.save(model, MODEL_DIR / "next_word_bigram.onnx")


if __name__ == "__main__":
    main()
