#ifndef MTLXRENDER_PASSIVE_FILM_H_
#define MTLXRENDER_PASSIVE_FILM_H_
#ifdef __cplusplus
extern "C" {
#endif

/* Conservative-mode channel model: lossless film, passive complex substrate,
 * s/p average at 620/550/460 nm. This deliberately differs from the reference
 * CIE-fit Airy approximation. cosTheta must be in (0,1], eta and film_ior must
 * be positive, extinction/thickness nonnegative; all inputs must be finite.
 * Returns zero on unsupported/degenerate arithmetic and leaves out unchanged.
 * Successful channels are in [0,1]. Compile without fast-math/contraction.
 * The exact admittance/passivity model is in MtlxSpec/ThinFilm{Passivity,Substrate}.
 * Native floating-point agreement is a separate tested boundary. */
int mtlx_passive_film(float cosTheta, const float eta[3], const float extinction[3],
                      float thickness_nm, float film_ior, float out[3]);

#ifdef __cplusplus
}
#endif
#endif
