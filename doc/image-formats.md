# Image format support

The image writer supports BMP, PNG, JPEG, OpenEXR, TIFF/DNG, and QOI when the
corresponding CMake option or bundled codec is enabled. Format autodetection for
file output uses the filename extension; memory output chooses EXR for float or
more-than-16-bit images and PNG otherwise.

TIFF/DNG writing uses the bundled TinyDNG writer and emits baseline contiguous
images. It supports 1–4 channels and 8-, 16-, or 32-bit samples, including
unsigned, signed, and IEEE floating-point sample formats. TIFF loading also
supports classic TIFF and BigTIFF; tiled TIFF loading requires the optional
system libtiff path.

TIFF/DNG loading also accepts packed 10/12/14-bit integer samples in both
chunky and plane-separated layouts; these are expanded to 16-bit samples
because the public `Image` representation is byte-addressable. Standard
strip-based TIFFs with padded rows and separate planar channels are decoded
through libtiff; the bundled fallback also handles chunky and planar PackBits
strips. Standard
8/16/32-bit tiled layouts are also decoded,
including uncompressed, LZW, Deflate/ZIP, and PackBits tiles; unusual tile
organizations and other compression schemes remain unsupported.
`LoadImageLayersFromMemory` preserves all decoded TIFF/DNG directories for
callers that need them; the regular single-image API still selects the largest
directory for compatibility. Unsupported bit depths and malformed/truncated
payloads fail with an explicit error rather than silently selecting another
encoder.

`WriteImageLayersToMemory` writes a vector of images as chained baseline TIFF
directories (also accepted when the requested format is `DNG`). Layers may
have different dimensions, but each layer must use the supported 1–4 channel,
8/16/32-bit byte-addressable representation.

Applications can register `SetImageLoader` and `SetImageInfoLoader` callbacks
for formats supplied by the host application. A registered callback is tried
before built-in decoders; returning `false` delegates to the normal format
detectors. Passing `nullptr` unregisters a callback. This is also the supported
path for builds configured with `LIGHTUSD_NO_BUILTIN_IMAGE_LOADER`.
