# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation.  All rights reserved.
# Licensed under the MIT License.  See License.txt in the project root for
# license information.
# --------------------------------------------------------------------------
import os
import shutil
import unittest

import numpy as np
import pytest
from parity_utilities import find_transformers_source

if find_transformers_source():
    from compare_bert_results import run_test
    from fusion_options import FusionOptions
    from optimizer import optimize_model
else:
    from onnxruntime.transformers.compare_bert_results import run_test
    from onnxruntime.transformers.fusion_options import FusionOptions
    from onnxruntime.transformers.optimizer import optimize_model

if find_transformers_source(["models", "stable_diffusion"]):
    from optimize_pipeline import main as optimize_stable_diffusion
else:
    from onnxruntime.transformers.models.stable_diffusion.optimize_pipeline import main as optimize_stable_diffusion


TINY_MODELS = {
    "stable-diffusion": "hf-internal-testing/tiny-stable-diffusion-torch",
    "stable-diffusion-xl": "echarlaix/tiny-random-stable-diffusion-xl",
    "stable-diffusion-3": "optimum-internal-testing/tiny-random-stable-diffusion-3",
    "flux": "tlwu/tiny-random-flux",
}


class TestStableDiffusionOptimization(unittest.TestCase):
    def verify_node_count(self, onnx_model, expected_node_count, test_name):
        for op_type, count in expected_node_count.items():
            if len(onnx_model.get_nodes_by_op_type(op_type)) != count:
                print(f"Counters is not expected in test: {test_name}")
                for op, counter in expected_node_count.items():
                    print(f"{op}: {len(onnx_model.get_nodes_by_op_type(op))} expected={counter}")

                self.assertEqual(len(onnx_model.get_nodes_by_op_type(op_type)), count)

    def verify_clip_optimizer(self, clip_onnx_path, optimized_clip_onnx_path, expected_counters, float16=False):
        fusion_options = FusionOptions("clip")
        m = optimize_model(
            clip_onnx_path,
            model_type="clip",
            num_heads=0,
            hidden_size=0,
            opt_level=0,
            optimization_options=fusion_options,
            use_gpu=True,
        )
        self.verify_node_count(m, expected_counters, "test_clip")

        if float16:
            m.convert_float_to_float16(
                keep_io_types=True,
            )
        print(m.get_operator_statistics())
        m.save_model_to_file(optimized_clip_onnx_path)

        threshold = 1e-2 if float16 else 3e-3
        max_abs_diff, passed = run_test(
            clip_onnx_path,
            optimized_clip_onnx_path,
            output_dir=None,
            batch_size=1,
            sequence_length=77,
            use_gpu=True,
            test_cases=10,
            seed=1,
            verbose=False,
            rtol=1e-1,
            atol=threshold,
            input_ids_name="input_ids",
            segment_ids_name=None,
            input_mask_name=None,
            mask_type=0,
        )

        self.assertLess(max_abs_diff, threshold)
        self.assertTrue(passed)

    @pytest.mark.slow
    def test_clip_sd(self):
        save_directory = "tiny-random-stable-diffusion"
        if os.path.exists(save_directory):
            shutil.rmtree(save_directory, ignore_errors=True)

        model_type = "stable-diffusion"
        model_name = TINY_MODELS[model_type]

        from optimum.onnxruntime import ORTStableDiffusionPipeline  # noqa: PLC0415

        base = ORTStableDiffusionPipeline.from_pretrained(model_name, export=True)
        base.save_pretrained(save_directory)

        clip_onnx_path = os.path.join(save_directory, "text_encoder", "model.onnx")
        optimized_clip_onnx_path = os.path.join(save_directory, "text_encoder", "opt.onnx")
        self.verify_clip_optimizer(
            clip_onnx_path,
            optimized_clip_onnx_path,
            expected_counters={
                "EmbedLayerNormalization": 0,
                "Attention": 5,
                "SkipLayerNormalization": 10,
                "LayerNormalization": 1,
                "Gelu": 0,
                "BiasGelu": 0,
            },
            float16=True,
        )


class TestStableDiffusionOrFluxPipelineOptimization(unittest.TestCase):
    def verify_pipeline_optimization(
        self,
        model_name,
        export_onnx_dir,
        optimized_onnx_dir,
        expected_op_counters,
        is_float16,
        atol,
        disable_group_norm=False,
    ):
        from optimum.onnxruntime import ORTPipelineForText2Image  # noqa: PLC0415

        if os.path.exists(export_onnx_dir):
            shutil.rmtree(export_onnx_dir, ignore_errors=True)

        baseline = ORTPipelineForText2Image.from_pretrained(model_name, export=True, provider="CUDAExecutionProvider")
        if not os.path.exists(export_onnx_dir):
            baseline.save_pretrained(export_onnx_dir)

        argv = [
            "--input",
            export_onnx_dir,
            "--output",
            optimized_onnx_dir,
            "--overwrite",
            "--disable_bias_splitgelu",
        ]

        if disable_group_norm:
            argv.append("--disable_group_norm")

        if is_float16:
            argv.append("--float16")

        op_counters = optimize_stable_diffusion(argv)
        print(op_counters)

        for name in expected_op_counters:
            self.assertIn(name, op_counters)
            for op, count in expected_op_counters[name].items():
                self.assertIn(op, op_counters[name])
                self.assertEqual(op_counters[name][op], count, f"Expected {count} {op} in {name}")

        treatment = ORTPipelineForText2Image.from_pretrained(optimized_onnx_dir, provider="CUDAExecutionProvider")
        batch_size, num_images_per_prompt, height, width = 1, 1, 64, 64
        inputs = {
            "prompt": ["starry night by van gogh"] * batch_size,
            "num_inference_steps": 20,
            "num_images_per_prompt": num_images_per_prompt,
            "height": height,
            "width": width,
            "output_type": "np",
        }

        seed = 123
        np.random.seed(seed)
        import torch  # noqa: PLC0415

        baseline_outputs = baseline(**inputs, generator=torch.Generator(device="cuda").manual_seed(seed))

        np.random.seed(seed)
        treatment_outputs = treatment(**inputs, generator=torch.Generator(device="cuda").manual_seed(seed))

        self.assertTrue(np.allclose(baseline_outputs.images[0], treatment_outputs.images[0], atol=atol))

    @pytest.mark.slow
    def test_sd(self):
        """This tests optimization of stable diffusion 1.x pipeline"""
        model_name = TINY_MODELS["stable-diffusion"]

        expected_op_counters = {
            "unet": {
                "Attention": 6,
                "MultiHeadAttention": 6,
                "LayerNormalization": 6,
                "SkipLayerNormalization": 12,
                "BiasSplitGelu": 0,
                "GroupNorm": 0,
                "SkipGroupNorm": 0,
                "NhwcConv": 47,
                "BiasAdd": 0,
            },
            "vae_encoder": {"Attention": 0, "GroupNorm": 0, "SkipGroupNorm": 0, "NhwcConv": 13},
            "vae_decoder": {"Attention": 0, "GroupNorm": 0, "SkipGroupNorm": 0, "NhwcConv": 17},
            "text_encoder": {
                "Attention": 5,
                "Gelu": 0,
                "LayerNormalization": 1,
                "QuickGelu": 5,
                "BiasGelu": 0,
                "SkipLayerNormalization": 10,
            },
        }

        export_onnx_dir = "tiny-random-sd"
        optimized_onnx_dir = "tiny-random-sd-optimized-fp32"
        # Disable GroupNorm due to limitation of current cuda kernel implementation.
        self.verify_pipeline_optimization(
            model_name,
            export_onnx_dir,
            optimized_onnx_dir,
            expected_op_counters,
            is_float16=False,
            atol=5e-3,
            disable_group_norm=True,
        )

        expected_op_counters["unet"].update({"Attention": 0, "MultiHeadAttention": 12})
        optimized_onnx_dir = "tiny-random-sd-optimized-fp16"
        self.verify_pipeline_optimization(
            model_name,
            export_onnx_dir,
            optimized_onnx_dir,
            expected_op_counters,
            is_float16=True,
            atol=5e-2,
            disable_group_norm=True,
        )

    @pytest.mark.slow
    def test_sdxl(self):
        """This tests optimization of SDXL pipeline"""
        model_name = TINY_MODELS["stable-diffusion-xl"]

        expected_op_counters = {
            "unet": {
                "Attention": 12,
                "MultiHeadAttention": 12,
                "LayerNormalization": 6,
                "SkipLayerNormalization": 30,
                "BiasSplitGelu": 0,
                "GroupNorm": 0,
                "SkipGroupNorm": 0,
                "NhwcConv": 35,
                "BiasAdd": 0,
            },
            "vae_encoder": {"Attention": 0, "GroupNorm": 0, "SkipGroupNorm": 0, "NhwcConv": 13},
            "vae_decoder": {"Attention": 0, "GroupNorm": 0, "SkipGroupNorm": 0, "NhwcConv": 17},
            "text_encoder": {
                "Attention": 5,
                "Gelu": 0,
                "LayerNormalization": 1,
                "QuickGelu": 0,
                "BiasGelu": 5,
                "SkipLayerNormalization": 10,
            },
            "text_encoder_2": {
                "Attention": 5,
                "Gelu": 0,
                "LayerNormalization": 1,
                "QuickGelu": 0,
                "BiasGelu": 5,
                "SkipLayerNormalization": 10,
            },
        }

        export_onnx_dir = "tiny-random-sdxl"
        optimized_onnx_dir = "tiny-random-sdxl-optimized-fp32"
        # Disable GroupNorm due to limitation of current cuda kernel implementation.
        self.verify_pipeline_optimization(
            model_name,
            export_onnx_dir,
            optimized_onnx_dir,
            expected_op_counters,
            is_float16=False,
            atol=5e-3,
            disable_group_norm=True,
        )

        expected_op_counters["unet"].update({"Attention": 0, "MultiHeadAttention": 24})
        optimized_onnx_dir = "tiny-random-sdxl-optimized-fp16"
        self.verify_pipeline_optimization(
            model_name,
            export_onnx_dir,
            optimized_onnx_dir,
            expected_op_counters,
            is_float16=True,
            atol=5e-2,
            disable_group_norm=True,
        )

    @pytest.mark.slow
    def test_sd3(self):
        """This tests optimization of stable diffusion 3 pipeline"""
        model_name = TINY_MODELS["stable-diffusion-3"]

        expected_op_counters = {
            "transformer": {
                "FastGelu": 3,
                "MultiHeadAttention": 2,
                "LayerNormalization": 8,
                "SimplifiedLayerNormalization": 0,
            },
            "vae_encoder": {"Attention": 0, "GroupNorm": 10, "SkipGroupNorm": 3, "NhwcConv": 17},
            "vae_decoder": {"Attention": 0, "GroupNorm": 14, "SkipGroupNorm": 7, "NhwcConv": 25},
            "text_encoder": {
                "Attention": 2,
                "Gelu": 0,
                "LayerNormalization": 1,
                "QuickGelu": 2,
                "SkipLayerNormalization": 4,
            },
            "text_encoder_2": {
                "Attention": 2,
                "Gelu": 0,
                "LayerNormalization": 1,
                "QuickGelu": 0,
                "SkipLayerNormalization": 4,
            },
            "text_encoder_3": {
                "Attention": 2,
                "MultiHeadAttention": 0,
                "Gelu": 0,
                "FastGelu": 2,
                "BiasGelu": 0,
                "GemmFastGelu": 0,
                "LayerNormalization": 0,
                "SimplifiedLayerNormalization": 2,
                "SkipLayerNormalization": 0,
                "SkipSimplifiedLayerNormalization": 3,
            },
        }

        export_onnx_dir = "tiny-random-stable-diffusion-3"
        optimized_onnx_dir = "tiny-random-stable-diffusion-3-optimized-fp32"
        self.verify_pipeline_optimization(
            model_name, export_onnx_dir, optimized_onnx_dir, expected_op_counters, is_float16=False, atol=5e-3
        )

        optimized_onnx_dir = "tiny-random-stable-diffusion-3-optimized-fp16"
        self.verify_pipeline_optimization(
            model_name, export_onnx_dir, optimized_onnx_dir, expected_op_counters, is_float16=True, atol=5e-2
        )

    @pytest.mark.slow
    def test_flux(self):
        """This tests optimization of flux pipeline"""
        model_name = TINY_MODELS["flux"]

        expected_op_counters = {
            "transformer": {
                "FastGelu": 8,
                "MultiHeadAttention": 6,
                "LayerNormalization": 13,
                "SimplifiedLayerNormalization": 16,
            },
            "vae_encoder": {"Attention": 0, "GroupNorm": 10, "SkipGroupNorm": 3, "NhwcConv": 17},
            "vae_decoder": {"Attention": 0, "GroupNorm": 14, "SkipGroupNorm": 7, "NhwcConv": 25},
            "text_encoder": {
                "Attention": 2,
                "Gelu": 0,
                "LayerNormalization": 1,
                "QuickGelu": 2,
                "SkipLayerNormalization": 4,
            },
            "text_encoder_2": {
                "Attention": 2,
                "MultiHeadAttention": 0,
                "Gelu": 0,
                "FastGelu": 2,
                "BiasGelu": 0,
                "GemmFastGelu": 0,
                "LayerNormalization": 0,
                "SimplifiedLayerNormalization": 2,
                "SkipLayerNormalization": 0,
                "SkipSimplifiedLayerNormalization": 3,
            },
        }

        export_onnx_dir = "tiny-random-flux"
        optimized_onnx_dir = "tiny-random-flux-optimized-fp32"
        self.verify_pipeline_optimization(
            model_name, export_onnx_dir, optimized_onnx_dir, expected_op_counters, is_float16=False, atol=1e-3
        )

        optimized_onnx_dir = "tiny-random-flux-optimized-fp16"
        self.verify_pipeline_optimization(
            model_name, export_onnx_dir, optimized_onnx_dir, expected_op_counters, is_float16=True, atol=5e-2
        )


if __name__ == "__main__":
    unittest.main()
