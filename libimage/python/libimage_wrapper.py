"""
libimage_wrapper.py - Python ctypes wrapper for the canonical int32 libimage codec.

API notes:
  - jpeg_params_t uses subsampling + flags
  - jpeg_compressed_t persists colorspace, subsampling and flags
  - minimal frame transport uses a 3-byte header [magic][len_hi][len_lo]
  - decode context for frames is supplied by the caller
  - Grayscale: cb/cr pointers may be NULL

Usage:
    from libimage_wrapper import LibImage

    lib = LibImage()
    rgb = lib.decompress(y_q, cb_q, cr_q,
                         width, height,
                         quality_factor=2.0,
                         dct_method="loeffler")

    result = lib.compress(rgb_data, width, height,
                          quality_factor=2.0,
                          dct_method="loeffler")
"""

import ctypes
import numpy as np
from pathlib import Path

# ======================== C Enum Values ========================

JPEG_SUCCESS = 0
JPEG_ERROR_NULL_POINTER = -1
JPEG_ERROR_INVALID_DIMENSIONS = -2
JPEG_ERROR_ALLOCATION_FAILED = -3
JPEG_ERROR_INVALID_METHOD = -4
JPEG_ERROR_INVALID_COLORSPACE = -5
JPEG_ERROR_INVALID_FRAME = -6
JPEG_ERROR_BUFFER_TOO_SMALL = -7

JPEG_DCT_LOEFFLER = 0
JPEG_DCT_MATRIX = 1
JPEG_DCT_RDCT = 2
JPEG_DCT_SILVEIRA_J3 = 3
JPEG_DCT_SILVEIRA_J7 = 4
JPEG_DCT_IDENTITY = 5

JPEG_COLORSPACE_RGB = 0
JPEG_COLORSPACE_GRAYSCALE = 1

JPEG_SUBSAMP_444 = 0
JPEG_SUBSAMP_422 = 1
JPEG_SUBSAMP_420 = 2

JPEG_FLAG_SKIP_QUANTIZATION = (1 << 0)
JPEG_FLAG_KEEP_COEFFS = (1 << 1)

DCT_METHOD_MAP = {
    "loeffler":    JPEG_DCT_LOEFFLER,
    "matrix":      JPEG_DCT_MATRIX,
    "rdct":        JPEG_DCT_RDCT,
    "silveira_j3": JPEG_DCT_SILVEIRA_J3,
    "silveira_j7": JPEG_DCT_SILVEIRA_J7,
    "identity":    JPEG_DCT_IDENTITY,
}


# ======================== C Struct Definitions (v2) ========================

class _JpegImage(ctypes.Structure):
    _fields_ = [
        ("width",      ctypes.c_int32),
        ("height",     ctypes.c_int32),
        ("colorspace", ctypes.c_int32),
        ("data",       ctypes.POINTER(ctypes.c_uint8)),
    ]


class _JpegParams(ctypes.Structure):
    _fields_ = [
        ("quality_factor", ctypes.c_float),
        ("dct_method",     ctypes.c_int32),
        ("subsampling",    ctypes.c_int32),
        ("flags",          ctypes.c_uint32),
    ]


class _JpegCompressed(ctypes.Structure):
    _fields_ = [
        ("width",               ctypes.c_int32),
        ("height",              ctypes.c_int32),
        ("quality_factor",      ctypes.c_float),
        ("dct_method",          ctypes.c_int32),
        ("colorspace",          ctypes.c_int32),
        ("subsampling",         ctypes.c_int32),
        ("flags",               ctypes.c_uint32),
        ("num_blocks_y",        ctypes.c_int32),
        ("num_blocks_chroma",   ctypes.c_int32),
        ("y_coeffs",            ctypes.POINTER(ctypes.c_int32)),
        ("y_quantized",         ctypes.POINTER(ctypes.c_int32)),
        ("cb_coeffs",           ctypes.POINTER(ctypes.c_int32)),
        ("cb_quantized",        ctypes.POINTER(ctypes.c_int32)),
        ("cr_coeffs",           ctypes.POINTER(ctypes.c_int32)),
        ("cr_quantized",        ctypes.POINTER(ctypes.c_int32)),
        # profiling outputs written by jpeg_decompress (must match jpeg_codec.h)
        ("dct_kernel_us",       ctypes.c_uint32),
        ("idct_kernel_us",      ctypes.c_uint32),
        ("dct_kernel_calls",    ctypes.c_uint32),
        ("idct_kernel_calls",   ctypes.c_uint32),
    ]


# ======================== LibImage Class ========================

class LibImage:
    """High-level wrapper around the C libimage shared library (v2 API)."""

    def __init__(self, lib_path: str | Path | None = None):
        if lib_path is None:
            lib_path = Path(__file__).resolve().parent.parent / "bin" / "libimage.so"

        lib_path = Path(lib_path)
        if not lib_path.exists():
            raise FileNotFoundError(
                f"libimage.so not found at {lib_path}. "
                f"Build it with:  cd libimage && make shared"
            )

        self._lib = ctypes.CDLL(str(lib_path))
        self._setup_prototypes()

    def _setup_prototypes(self):
        L = self._lib

        L.jpeg_compress.restype = ctypes.c_int32
        L.jpeg_compress.argtypes = [
            ctypes.POINTER(_JpegImage),
            ctypes.POINTER(_JpegParams),
            ctypes.POINTER(ctypes.POINTER(_JpegCompressed)),
        ]

        L.jpeg_decompress.restype = ctypes.c_int32
        L.jpeg_decompress.argtypes = [
            ctypes.POINTER(_JpegCompressed),
            ctypes.POINTER(ctypes.POINTER(_JpegImage)),
        ]

        L.jpeg_free_compressed.restype = None
        L.jpeg_free_compressed.argtypes = [ctypes.POINTER(_JpegCompressed)]

        L.jpeg_free_image.restype = None
        L.jpeg_free_image.argtypes = [ctypes.POINTER(_JpegImage)]

        L.jpeg_error_string.restype = ctypes.c_char_p
        L.jpeg_error_string.argtypes = [ctypes.c_int32]

        L.jpeg_version.restype = ctypes.c_char_p
        L.jpeg_version.argtypes = []

        L.jpeg_frame_encode.restype = ctypes.c_int32
        L.jpeg_frame_encode.argtypes = [
            ctypes.POINTER(_JpegCompressed),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_int32,
        ]

        L.jpeg_frame_decode.restype = ctypes.c_int32
        L.jpeg_frame_decode.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_int32,
            ctypes.POINTER(_JpegCompressed),
        ]

        L.jpeg_frame_alloc_compressed.restype = ctypes.POINTER(_JpegCompressed)
        L.jpeg_frame_alloc_compressed.argtypes = [
            ctypes.c_int32, ctypes.c_int32,
            ctypes.c_int32, ctypes.c_int32,  # subsampling, colorspace
        ]

        L.jpeg_cobs_encode.restype = ctypes.c_int32
        L.jpeg_cobs_encode.argtypes = [
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32,
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32,
        ]

        L.jpeg_cobs_decode.restype = ctypes.c_int32
        L.jpeg_cobs_decode.argtypes = [
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32,
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32,
        ]

        L.jpeg_rle_encode.restype = ctypes.c_int32
        L.jpeg_rle_encode.argtypes = [
            ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32,
        ]

    # ----------------------------------------------------------------
    #  decompress
    # ----------------------------------------------------------------
    def decompress(
        self,
        y_quantized: np.ndarray,
        cb_quantized: np.ndarray | None,
        cr_quantized: np.ndarray | None,
        width: int,
        height: int,
        quality_factor: float,
        dct_method: str,
        colorspace: int = JPEG_COLORSPACE_RGB,
        subsampling: int = JPEG_SUBSAMP_444,
        flags: int = 0,
    ) -> np.ndarray:
        method_enum = self._resolve_method(dct_method)
        nb_y = y_quantized.shape[0]

        y_flat = np.ascontiguousarray(y_quantized.flatten(), dtype=np.int32)

        comp = _JpegCompressed()
        comp.width = width
        comp.height = height
        comp.quality_factor = quality_factor
        comp.dct_method = method_enum
        comp.colorspace = colorspace
        comp.subsampling = subsampling
        comp.flags = flags
        comp.num_blocks_y = nb_y
        comp.y_coeffs = None
        comp.cb_coeffs = None
        comp.cr_coeffs = None
        comp.y_quantized = y_flat.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))

        if cb_quantized is not None and cr_quantized is not None:
            cb_flat = np.ascontiguousarray(cb_quantized.flatten(), dtype=np.int32)
            cr_flat = np.ascontiguousarray(cr_quantized.flatten(), dtype=np.int32)
            comp.num_blocks_chroma = cb_quantized.shape[0]
            comp.cb_quantized = cb_flat.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))
            comp.cr_quantized = cr_flat.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))
        else:
            cb_flat = cr_flat = None  # keep refs alive
            comp.num_blocks_chroma = 0
            comp.cb_quantized = None
            comp.cr_quantized = None

        img_ptr = ctypes.POINTER(_JpegImage)()
        err = self._lib.jpeg_decompress(ctypes.byref(comp), ctypes.byref(img_ptr))
        if err != JPEG_SUCCESS:
            raise RuntimeError(
                f"jpeg_decompress failed: "
                f"{self._lib.jpeg_error_string(err).decode()} (code {err})"
            )

        img = img_ptr.contents
        channels = 1 if colorspace == JPEG_COLORSPACE_GRAYSCALE else 3
        n_bytes = img.width * img.height * channels
        rgb = np.ctypeslib.as_array(img.data, shape=(n_bytes,)).copy()
        self._lib.jpeg_free_image(img_ptr)

        if channels == 1:
            return rgb.reshape(height, width)
        return rgb.reshape(height, width, 3)

    # ----------------------------------------------------------------
    #  compress
    # ----------------------------------------------------------------
    def compress(
        self,
        rgb_data: np.ndarray,
        width: int,
        height: int,
        quality_factor: float,
        dct_method: str,
        colorspace: int = JPEG_COLORSPACE_RGB,
        subsampling: int = JPEG_SUBSAMP_444,
        skip_quantization: bool = False,
        keep_coeffs: bool = True,
    ) -> dict:
        method_enum = self._resolve_method(dct_method)

        rgb_flat = np.ascontiguousarray(rgb_data.flatten(), dtype=np.uint8)

        img = _JpegImage()
        img.width = width
        img.height = height
        img.colorspace = colorspace
        img.data = rgb_flat.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))

        flags = 0
        if skip_quantization:
            flags |= JPEG_FLAG_SKIP_QUANTIZATION
        if keep_coeffs:
            flags |= JPEG_FLAG_KEEP_COEFFS

        params = _JpegParams()
        params.quality_factor = quality_factor
        params.dct_method = method_enum
        params.subsampling = subsampling
        params.flags = flags

        comp_ptr = ctypes.POINTER(_JpegCompressed)()
        err = self._lib.jpeg_compress(
            ctypes.byref(img), ctypes.byref(params), ctypes.byref(comp_ptr)
        )
        if err != JPEG_SUCCESS:
            raise RuntimeError(
                f"jpeg_compress failed: "
                f"{self._lib.jpeg_error_string(err).decode()} (code {err})"
            )

        comp = comp_ptr.contents
        nb_y = comp.num_blocks_y
        nb_c = comp.num_blocks_chroma
        bs_y = nb_y * 64
        bs_c = nb_c * 64

        result = {
            "num_blocks": nb_y,
            "num_blocks_chroma": nb_c,
            "y_quantized": np.ctypeslib.as_array(comp.y_quantized, shape=(bs_y,)).copy().reshape(nb_y, 64),
        }

        if nb_c > 0 and comp.cb_quantized and comp.cr_quantized:
            result["cb_quantized"] = np.ctypeslib.as_array(comp.cb_quantized, shape=(bs_c,)).copy().reshape(nb_c, 64)
            result["cr_quantized"] = np.ctypeslib.as_array(comp.cr_quantized, shape=(bs_c,)).copy().reshape(nb_c, 64)

        if comp.y_coeffs:
            result["y_coeffs"] = np.ctypeslib.as_array(comp.y_coeffs, shape=(bs_y,)).copy().reshape(nb_y, 64)
        if nb_c > 0 and comp.cb_coeffs:
            result["cb_coeffs"] = np.ctypeslib.as_array(comp.cb_coeffs, shape=(bs_c,)).copy().reshape(nb_c, 64)
            result["cr_coeffs"] = np.ctypeslib.as_array(comp.cr_coeffs, shape=(bs_c,)).copy().reshape(nb_c, 64)

        self._lib.jpeg_free_compressed(comp_ptr)
        return result

    # ----------------------------------------------------------------
    #  full pipeline: compress + decompress (round-trip)
    # ----------------------------------------------------------------
    def process_image(
        self,
        rgb_data: np.ndarray,
        width: int,
        height: int,
        quality_factor: float,
        dct_method: str,
        subsampling: int = JPEG_SUBSAMP_444,
    ) -> dict:
        comp = self.compress(rgb_data, width, height, quality_factor, dct_method,
                             subsampling=subsampling)
        recon = self.decompress(
            comp["y_quantized"],
            comp.get("cb_quantized"), comp.get("cr_quantized"),
            width, height, quality_factor, dct_method,
            subsampling=subsampling,
        )
        comp["recon_rgb"] = recon
        return comp

    # ----------------------------------------------------------------
    #  helpers
    # ----------------------------------------------------------------
    @staticmethod
    def _resolve_method(name: str) -> int:
        m = DCT_METHOD_MAP.get(name)
        if m is None:
            raise ValueError(
                f"Unknown DCT method '{name}'. "
                f"Valid: {list(DCT_METHOD_MAP.keys())}"
            )
        return m

    # ----------------------------------------------------------------
    #  frame_encode_to_bytes
    # ----------------------------------------------------------------
    def frame_encode_to_bytes(self, compressed: dict,
                              width: int = 0, height: int = 0,
                              quality_factor: float = 1.0,
                              dct_method: int = 0,
                              colorspace: int = JPEG_COLORSPACE_RGB,
                              subsampling: int = JPEG_SUBSAMP_444,
                              flags: int = 0) -> bytes:
        nb_y = compressed["num_blocks"]
        nb_c = compressed.get("num_blocks_chroma", nb_y)

        y_q = np.ascontiguousarray(compressed["y_quantized"].flatten(), dtype=np.int32)

        comp = _JpegCompressed()
        comp.width = width or compressed.get("width", 0)
        comp.height = height or compressed.get("height", 0)
        comp.quality_factor = quality_factor
        comp.dct_method = dct_method
        comp.colorspace = colorspace
        comp.subsampling = subsampling
        comp.flags = flags
        comp.num_blocks_y = nb_y
        comp.num_blocks_chroma = nb_c
        comp.y_coeffs = comp.cb_coeffs = comp.cr_coeffs = None
        comp.y_quantized = y_q.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))

        if "cb_quantized" in compressed and "cr_quantized" in compressed:
            cb_q = np.ascontiguousarray(compressed["cb_quantized"].flatten(), dtype=np.int32)
            cr_q = np.ascontiguousarray(compressed["cr_quantized"].flatten(), dtype=np.int32)
            comp.cb_quantized = cb_q.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))
            comp.cr_quantized = cr_q.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))
        else:
            cb_q = cr_q = None
            comp.cb_quantized = None
            comp.cr_quantized = None

        buf_size = max(nb_y, nb_c) * 64 * 4 * 3 + 256
        buf = (ctypes.c_uint8 * buf_size)()

        n = self._lib.jpeg_frame_encode(ctypes.byref(comp), buf, buf_size)
        if n < 0:
            raise RuntimeError("jpeg_frame_encode failed")

        return bytes(buf[:n])

    # ----------------------------------------------------------------
    #  frame_decode_from_bytes
    # ----------------------------------------------------------------
    def frame_decode_from_bytes(self, data: bytes,
                                width: int = 0, height: int = 0,
                                subsampling: int = JPEG_SUBSAMP_444,
                                colorspace: int = JPEG_COLORSPACE_RGB) -> dict:
        comp_ptr = self._lib.jpeg_frame_alloc_compressed(
            width, height, subsampling, colorspace)
        if not comp_ptr:
            raise MemoryError("jpeg_frame_alloc_compressed failed")

        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        consumed = self._lib.jpeg_frame_decode(buf, len(data), comp_ptr)
        if consumed < 0:
            self._lib.jpeg_free_compressed(comp_ptr)
            if consumed == -2:
                raise ValueError("Frame CRC mismatch")
            raise ValueError("jpeg_frame_decode failed")

        comp = comp_ptr.contents
        nb_y = comp.num_blocks_y
        nb_c = comp.num_blocks_chroma

        result = {
            "num_blocks": nb_y,
            "num_blocks_chroma": nb_c,
            "width": comp.width,
            "height": comp.height,
            "quality_factor": comp.quality_factor,
            "dct_method": comp.dct_method,
            "colorspace": comp.colorspace,
            "subsampling": comp.subsampling,
            "flags": comp.flags,
            "y_quantized": np.ctypeslib.as_array(
                comp.y_quantized, shape=(nb_y * 64,)).copy().reshape(nb_y, 64),
        }

        if nb_c > 0 and comp.cb_quantized and comp.cr_quantized:
            result["cb_quantized"] = np.ctypeslib.as_array(
                comp.cb_quantized, shape=(nb_c * 64,)).copy().reshape(nb_c, 64)
            result["cr_quantized"] = np.ctypeslib.as_array(
                comp.cr_quantized, shape=(nb_c * 64,)).copy().reshape(nb_c, 64)

        self._lib.jpeg_free_compressed(comp_ptr)
        return result

    def version(self) -> str:
        return self._lib.jpeg_version().decode()
