"""
libimage_wrapper.py — Python ctypes wrapper for the C libimage JPEG codec.

Provides direct access to the C implementation so that PC-side decompression
produces bit-identical results to the ESP32 firmware.

Usage:
    from libimage_wrapper import LibImage

    lib = LibImage()                          # loads libimage.so
    rgb = lib.decompress(y_q, cb_q, cr_q,
                         width, height,
                         quality_factor=2.0,
                         dct_method="loeffler")

    result = lib.compress(rgb_data, width, height,
                          quality_factor=2.0,
                          dct_method="loeffler")
"""

import ctypes
import ctypes.util
import numpy as np
from pathlib import Path

# ======================== C Enum Values ========================

JPEG_SUCCESS = 0
JPEG_ERROR_NULL_POINTER = -1
JPEG_ERROR_INVALID_DIMENSIONS = -2
JPEG_ERROR_ALLOCATION_FAILED = -3
JPEG_ERROR_INVALID_METHOD = -4

JPEG_DCT_LOEFFLER = 0
JPEG_DCT_MATRIX = 1
JPEG_DCT_APPROX = 2
JPEG_DCT_IDENTITY = 3

JPEG_COLORSPACE_RGB = 0
JPEG_COLORSPACE_GRAYSCALE = 1

DCT_METHOD_MAP = {
    "loeffler":    JPEG_DCT_LOEFFLER,
    "matrix":      JPEG_DCT_MATRIX,
    "approx":      JPEG_DCT_APPROX,
    "approximate": JPEG_DCT_APPROX,
    "identity":    JPEG_DCT_IDENTITY,
}


# ======================== C Struct Definitions ========================

class _JpegImage(ctypes.Structure):
    _fields_ = [
        ("width",      ctypes.c_int32),
        ("height",     ctypes.c_int32),
        ("colorspace", ctypes.c_int32),
        ("data",       ctypes.POINTER(ctypes.c_uint8)),
    ]


class _JpegParams(ctypes.Structure):
    _fields_ = [
        ("quality_factor",       ctypes.c_float),
        ("dct_method",           ctypes.c_int32),
        ("use_standard_tables",  ctypes.c_int32),
        ("skip_quantization",    ctypes.c_int32),
    ]


class _JpegCompressed(ctypes.Structure):
    _fields_ = [
        ("width",             ctypes.c_int32),
        ("height",            ctypes.c_int32),
        ("quality_factor",    ctypes.c_float),
        ("dct_method",        ctypes.c_int32),
        ("num_blocks_y",      ctypes.c_int32),
        ("num_blocks_chroma", ctypes.c_int32),
        ("y_coeffs",          ctypes.POINTER(ctypes.c_int32)),
        ("y_quantized",       ctypes.POINTER(ctypes.c_int32)),
        ("cb_coeffs",         ctypes.POINTER(ctypes.c_int32)),
        ("cb_quantized",      ctypes.POINTER(ctypes.c_int32)),
        ("cr_coeffs",         ctypes.POINTER(ctypes.c_int32)),
        ("cr_quantized",      ctypes.POINTER(ctypes.c_int32)),
    ]


# ======================== LibImage Class ========================

class LibImage:
    """High-level wrapper around the C libimage shared library."""

    def __init__(self, lib_path: str | Path | None = None):
        if lib_path is None:
            # Default: look next to this file's parent → bin/libimage.so
            lib_path = Path(__file__).resolve().parent.parent / "bin" / "libimage.so"

        lib_path = Path(lib_path)
        if not lib_path.exists():
            raise FileNotFoundError(
                f"libimage.so not found at {lib_path}. "
                f"Build it with:  cd libimage && make shared"
            )

        self._lib = ctypes.CDLL(str(lib_path))
        self._setup_prototypes()

    # ---- internal: set C function signatures ----
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

    # ----------------------------------------------------------------
    #  decompress: quantized coefficients → RGB image
    # ----------------------------------------------------------------
    def decompress(
        self,
        y_quantized: np.ndarray,
        cb_quantized: np.ndarray,
        cr_quantized: np.ndarray,
        width: int,
        height: int,
        quality_factor: float,
        dct_method: str,
    ) -> np.ndarray:
        """
        Decompress quantized DCT coefficients to an RGB image using the
        exact same C code that runs on the ESP32.

        Parameters
        ----------
        y_quantized  : (num_blocks, 64) int32
        cb_quantized : (num_blocks, 64) int32
        cr_quantized : (num_blocks, 64) int32
        width, height: image dimensions
        quality_factor: quantization scale k (1.0–8.0)
        dct_method   : "loeffler" | "matrix" | "approx" | "identity"

        Returns
        -------
        np.ndarray : (height, width, 3) uint8 RGB image
        """
        method_enum = self._resolve_method(dct_method)
        nb = y_quantized.shape[0]

        # Keep numpy arrays alive while C accesses their data
        y_flat  = np.ascontiguousarray(y_quantized.flatten(),  dtype=np.int32)
        cb_flat = np.ascontiguousarray(cb_quantized.flatten(), dtype=np.int32)
        cr_flat = np.ascontiguousarray(cr_quantized.flatten(), dtype=np.int32)

        comp = _JpegCompressed()
        comp.width = width
        comp.height = height
        comp.quality_factor = quality_factor
        comp.dct_method = method_enum
        comp.num_blocks_y = nb
        comp.num_blocks_chroma = nb
        comp.y_coeffs  = None
        comp.cb_coeffs = None
        comp.cr_coeffs = None
        comp.y_quantized  = y_flat.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))
        comp.cb_quantized = cb_flat.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))
        comp.cr_quantized = cr_flat.ctypes.data_as(ctypes.POINTER(ctypes.c_int32))

        img_ptr = ctypes.POINTER(_JpegImage)()
        err = self._lib.jpeg_decompress(ctypes.byref(comp), ctypes.byref(img_ptr))
        if err != JPEG_SUCCESS:
            raise RuntimeError(
                f"jpeg_decompress failed: "
                f"{self._lib.jpeg_error_string(err).decode()} (code {err})"
            )

        img = img_ptr.contents
        n_bytes = img.width * img.height * 3
        rgb = np.ctypeslib.as_array(img.data, shape=(n_bytes,)).copy()

        # Free C-allocated image (NOT comp — numpy owns those buffers)
        self._lib.jpeg_free_image(img_ptr)

        return rgb.reshape(height, width, 3)

    # ----------------------------------------------------------------
    #  compress: RGB image → quantized coefficients
    # ----------------------------------------------------------------
    def compress(
        self,
        rgb_data: np.ndarray,
        width: int,
        height: int,
        quality_factor: float,
        dct_method: str,
        skip_quantization: bool = False,
    ) -> dict:
        """
        Compress an RGB image using the C libimage codec.

        Parameters
        ----------
        rgb_data : (height, width, 3) uint8
        width, height : image dimensions
        quality_factor : quantization scale k
        dct_method : "loeffler" | "matrix" | "approx" | "identity"

        Returns
        -------
        dict with:
            y_quantized, cb_quantized, cr_quantized : (nb, 64) int32
            y_coeffs, cb_coeffs, cr_coeffs           : (nb, 64) int32
            num_blocks : int
        """
        method_enum = self._resolve_method(dct_method)

        rgb_flat = np.ascontiguousarray(rgb_data.flatten(), dtype=np.uint8)

        img = _JpegImage()
        img.width = width
        img.height = height
        img.colorspace = JPEG_COLORSPACE_RGB
        img.data = rgb_flat.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))

        params = _JpegParams()
        params.quality_factor = quality_factor
        params.dct_method = method_enum
        params.use_standard_tables = 1
        params.skip_quantization = 1 if skip_quantization else 0

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
        nb = comp.num_blocks_y
        bs = nb * 64

        result = {
            "num_blocks": nb,
            "y_quantized":  np.ctypeslib.as_array(comp.y_quantized,  shape=(bs,)).copy().reshape(nb, 64),
            "cb_quantized": np.ctypeslib.as_array(comp.cb_quantized, shape=(bs,)).copy().reshape(nb, 64),
            "cr_quantized": np.ctypeslib.as_array(comp.cr_quantized, shape=(bs,)).copy().reshape(nb, 64),
        }

        if comp.y_coeffs:
            result["y_coeffs"]  = np.ctypeslib.as_array(comp.y_coeffs,  shape=(bs,)).copy().reshape(nb, 64)
            result["cb_coeffs"] = np.ctypeslib.as_array(comp.cb_coeffs, shape=(bs,)).copy().reshape(nb, 64)
            result["cr_coeffs"] = np.ctypeslib.as_array(comp.cr_coeffs, shape=(bs,)).copy().reshape(nb, 64)

        # Free ALL C-allocated memory
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
    ) -> dict:
        """
        Full compress → decompress pipeline.

        Returns dict with:
            recon_rgb : (H, W, 3) uint8
            y_quantized, cb_quantized, cr_quantized : (nb, 64) int32
            num_blocks : int
        """
        comp = self.compress(rgb_data, width, height, quality_factor, dct_method)
        recon = self.decompress(
            comp["y_quantized"], comp["cb_quantized"], comp["cr_quantized"],
            width, height, quality_factor, dct_method,
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

    def version(self) -> str:
        return self._lib.jpeg_version().decode()
