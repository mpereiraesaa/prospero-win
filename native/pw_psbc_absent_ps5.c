/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The Vulkan presentation backend records only barriers and buffer-to-image
 * copies; it never creates a pipeline.  ps5-vulkan's platform still names
 * its SPIR-V compiler, whose ~13 MiB would push the title image into the
 * fixed low range PE32 executables must occupy.  These definitions replace
 * the six compiler entry points so libpsbc.a is not linked: any shader
 * compilation fails closed with PSBC_RESULT_UNSUPPORTED_STAGE (2). */
#include <stddef.h>
#include <stdint.h>

enum { PSBC_RESULT_UNSUPPORTED_STAGE=2 };

int psbc_compile_shader(const uint32_t *spirv,size_t spirv_size,const void *opts,void *out)
{(void)spirv;(void)spirv_size;(void)opts;(void)out;return PSBC_RESULT_UNSUPPORTED_STAGE;}
int psbc_compile_geometry_pipeline(const uint32_t *vertex,size_t vertex_size,
    const uint32_t *geometry,size_t geometry_size,const void *opts,void *out)
{
    (void)vertex;(void)vertex_size;(void)geometry;(void)geometry_size;(void)opts;(void)out;
    return PSBC_RESULT_UNSUPPORTED_STAGE;
}
int psbc_compile_tess_geometry_pipeline(const uint32_t *control,size_t control_size,
    const uint32_t *evaluation,size_t evaluation_size,const uint32_t *geometry,
    size_t geometry_size,const void *opts,void *out)
{
    (void)control;(void)control_size;(void)evaluation;(void)evaluation_size;
    (void)geometry;(void)geometry_size;(void)opts;(void)out;
    return PSBC_RESULT_UNSUPPORTED_STAGE;
}
int psbc_compile_domain_pipeline(const uint32_t *control,size_t control_size,
    const uint32_t *evaluation,size_t evaluation_size,const void *opts,void *out)
{
    (void)control;(void)control_size;(void)evaluation;(void)evaluation_size;(void)opts;(void)out;
    return PSBC_RESULT_UNSUPPORTED_STAGE;
}
int psbc_compile_tess_pipeline(const uint32_t *vertex,size_t vertex_size,
    const uint32_t *control,size_t control_size,const uint32_t *evaluation,
    size_t evaluation_size,const void *opts,void *out)
{
    (void)vertex;(void)vertex_size;(void)control;(void)control_size;
    (void)evaluation;(void)evaluation_size;(void)opts;(void)out;
    return PSBC_RESULT_UNSUPPORTED_STAGE;
}
/* Nothing is ever allocated by the stubs above. */
void psbc_free_output(void *out){(void)out;}
