/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_command.h"
#include <string.h>
_Static_assert(sizeof(float)==4 && sizeof(int)==4 && sizeof(BOOL)==4, "D3D9 scalar ABI");
static float scalar_float(uint32_t bits)
{
    float value; memcpy(&value, &bits, sizeof(value)); return value;
}
static int32_t scalar_int(uint32_t bits)
{
    int32_t value; memcpy(&value, &bits, sizeof(value)); return value;
}
static void color(D3DCOLORVALUE *v, const uint32_t *p)
{
    v->r=scalar_float(p[0]);v->g=scalar_float(p[1]);
    v->b=scalar_float(p[2]);v->a=scalar_float(p[3]);
}
static uint32_t object_kind(uint32_t method)
{
    switch(method){
    case 37:case 39:return PW_D3D9_KIND_SURFACE;
    case 65:return PW_D3D9_KIND_TEXTURE_2D;
    case 87:return PW_D3D9_KIND_VERTEX_DECLARATION;
    case 92:return PW_D3D9_KIND_VERTEX_SHADER;
    case 100:return PW_D3D9_KIND_VERTEX_BUFFER;
    case 104:return PW_D3D9_KIND_INDEX_BUFFER;
    case 107:return PW_D3D9_KIND_PIXEL_SHADER;
    default:return 0;
    }
}
HRESULT pw_d3d9_native_command_dispatch(IDirect3DDevice9 *device,
        const struct pw_d3d9_command *c,pw_d3d9_command_acquire_fn acquire,void *context)
{
    const struct pw_d3d9_command_schema *schema;
    const uint32_t *a,*d;size_t bytes;unsigned i;void *object=NULL;HRESULT hr;
    union {
        D3DMATRIX matrix;D3DVIEWPORT9 viewport;D3DMATERIAL9 material;D3DLIGHT9 light;
        D3DCLIPSTATUS9 clip;RECT rect;PALETTEENTRY palette[256];D3DRECT clear[256];
        float f[1024];int integers[1024];BOOL booleans[1024];
    } n;
    if(!device || !c)return D3DERR_INVALIDCALL;
    schema=pw_d3d9_command_schema(c->method);
    if(!schema)return E_NOTIMPL;
    if(pw_d3d9_command_data_bytes(c->method,c->args,&bytes) || bytes!=c->data_bytes)
        return D3DERR_INVALIDCALL;
    if(schema->object_words && !object_kind(c->method))return E_NOTIMPL;
    a=c->args;d=c->data.words;
    if(schema->shape==PW_D3D9_DATA_BOOL)
        for(i=0;i<bytes/4;i++)if(d[i]>1)return D3DERR_INVALIDCALL;
    for(i=0;i<schema->words;i++)if(schema->object_words&(1u<<i)){
        if(a[i]){
            if(!acquire)return D3DERR_INVALIDCALL;
            hr=acquire(context,a[i],a[i+1],object_kind(c->method),device,&object);
            if(FAILED(hr))return hr;
            if(!object)return D3DERR_INVALIDCALL;
        }
        break;
    }
    switch(schema->shape){
    case PW_D3D9_DATA_MATRIX:
        for(i=0;i<16;i++)n.matrix.m[i/4][i%4]=scalar_float(d[i]);
        break;
    case PW_D3D9_DATA_VIEWPORT:
        n.viewport.X=d[0];n.viewport.Y=d[1];n.viewport.Width=d[2];n.viewport.Height=d[3];
        n.viewport.MinZ=scalar_float(d[4]);n.viewport.MaxZ=scalar_float(d[5]);break;
    case PW_D3D9_DATA_MATERIAL:
        color(&n.material.Diffuse,d);color(&n.material.Ambient,d+4);
        color(&n.material.Specular,d+8);color(&n.material.Emissive,d+12);
        n.material.Power=scalar_float(d[16]);break;
    case PW_D3D9_DATA_LIGHT:
        n.light.Type=d[0];color(&n.light.Diffuse,d+1);color(&n.light.Specular,d+5);color(&n.light.Ambient,d+9);
        n.light.Position.x=scalar_float(d[13]);n.light.Position.y=scalar_float(d[14]);n.light.Position.z=scalar_float(d[15]);
        n.light.Direction.x=scalar_float(d[16]);n.light.Direction.y=scalar_float(d[17]);n.light.Direction.z=scalar_float(d[18]);
        n.light.Range=scalar_float(d[19]);n.light.Falloff=scalar_float(d[20]);
        n.light.Attenuation0=scalar_float(d[21]);n.light.Attenuation1=scalar_float(d[22]);n.light.Attenuation2=scalar_float(d[23]);
        n.light.Theta=scalar_float(d[24]);n.light.Phi=scalar_float(d[25]);break;
    case PW_D3D9_DATA_CLIP:n.clip.ClipUnion=d[0];n.clip.ClipIntersection=d[1];break;
    case PW_D3D9_DATA_RECT:
        n.rect.left=scalar_int(d[0]);n.rect.top=scalar_int(d[1]);n.rect.right=scalar_int(d[2]);n.rect.bottom=scalar_int(d[3]);break;
    case PW_D3D9_DATA_PALETTE:
        for(i=0;i<256;i++){n.palette[i].peRed=c->data.bytes[4*i];n.palette[i].peGreen=c->data.bytes[4*i+1];
            n.palette[i].peBlue=c->data.bytes[4*i+2];n.palette[i].peFlags=c->data.bytes[4*i+3];}break;
    case PW_D3D9_DATA_CLEAR:
        for(i=0;i<a[0];i++){n.clear[i].x1=scalar_int(d[4*i]);n.clear[i].y1=scalar_int(d[4*i+1]);
            n.clear[i].x2=scalar_int(d[4*i+2]);n.clear[i].y2=scalar_int(d[4*i+3]);}break;
    case PW_D3D9_DATA_BOOL:for(i=0;i<bytes/4;i++)n.booleans[i]=d[i];break;
    case PW_D3D9_DATA_PLANE:case PW_D3D9_DATA_VECTOR4:
        if(c->method==96 || c->method==111)for(i=0;i<bytes/4;i++)n.integers[i]=scalar_int(d[i]);
        else for(i=0;i<bytes/4;i++)n.f[i]=scalar_float(d[i]);
        break;
    default:break;
    }
#define CALL(name,...) IDirect3DDevice9_##name(device, ##__VA_ARGS__)
    switch(c->method){
    case 3:hr=CALL(TestCooperativeLevel);break;
    case 5:hr=CALL(EvictManagedResources);break;
    case 20:hr=CALL(SetDialogBoxMode,a[0]);break;
    case 37:hr=CALL(SetRenderTarget,a[0],(IDirect3DSurface9 *)object);break;
    case 39:hr=CALL(SetDepthStencilSurface,(IDirect3DSurface9 *)object);break;
    case 41:hr=CALL(BeginScene);break;
    case 42:hr=CALL(EndScene);break;
    case 43:hr=CALL(Clear,a[0],a[0]?n.clear:NULL,a[1],a[2],scalar_float(a[3]),a[4]);break;
    case 44:hr=CALL(SetTransform,a[0],&n.matrix);break;
    case 46:hr=CALL(MultiplyTransform,a[0],&n.matrix);break;
    case 47:hr=CALL(SetViewport,&n.viewport);break;
    case 49:hr=CALL(SetMaterial,&n.material);break;
    case 51:hr=CALL(SetLight,a[0],&n.light);break;
    case 53:hr=CALL(LightEnable,a[0],a[1]);break;
    case 55:hr=CALL(SetClipPlane,a[0],n.f);break;
    case 57:hr=CALL(SetRenderState,a[0],a[1]);break;
    case 62:hr=CALL(SetClipStatus,&n.clip);break;
    case 65:hr=CALL(SetTexture,a[0],(IDirect3DBaseTexture9 *)object);break;
    case 67:hr=CALL(SetTextureStageState,a[0],a[1],a[2]);break;
    case 69:hr=CALL(SetSamplerState,a[0],a[1],a[2]);break;
    case 71:hr=CALL(SetPaletteEntries,a[0],n.palette);break;
    case 73:hr=CALL(SetCurrentTexturePalette,a[0]);break;
    case 75:hr=CALL(SetScissorRect,&n.rect);break;
    case 77:hr=CALL(SetSoftwareVertexProcessing,a[0]);break;
    case 79:hr=CALL(SetNPatchMode,scalar_float(a[0]));break;
    case 81:hr=CALL(DrawPrimitive,a[0],a[1],a[2]);break;
    case 82:hr=CALL(DrawIndexedPrimitive,a[0],scalar_int(a[1]),a[2],a[3],a[4],a[5]);break;
    case 87:hr=CALL(SetVertexDeclaration,(IDirect3DVertexDeclaration9 *)object);break;
    case 89:hr=CALL(SetFVF,a[0]);break;
    case 92:hr=CALL(SetVertexShader,(IDirect3DVertexShader9 *)object);break;
    case 94:hr=CALL(SetVertexShaderConstantF,a[0],n.f,a[1]);break;
    case 96:hr=CALL(SetVertexShaderConstantI,a[0],n.integers,a[1]);break;
    case 98:hr=CALL(SetVertexShaderConstantB,a[0],n.booleans,a[1]);break;
    case 100:hr=CALL(SetStreamSource,a[0],(IDirect3DVertexBuffer9 *)object,a[3],a[4]);break;
    case 102:hr=CALL(SetStreamSourceFreq,a[0],a[1]);break;
    case 104:hr=CALL(SetIndices,(IDirect3DIndexBuffer9 *)object);break;
    case 107:hr=CALL(SetPixelShader,(IDirect3DPixelShader9 *)object);break;
    case 109:hr=CALL(SetPixelShaderConstantF,a[0],n.f,a[1]);break;
    case 111:hr=CALL(SetPixelShaderConstantI,a[0],n.integers,a[1]);break;
    case 113:hr=CALL(SetPixelShaderConstantB,a[0],n.booleans,a[1]);break;
    default:hr=E_NOTIMPL;break;
    }
#undef CALL
    if(object)IUnknown_Release((IUnknown *)object);
    return hr;
}
