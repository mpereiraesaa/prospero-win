/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_getter.h"
#include <string.h>
#include "../pw_d3d9_factory_wire.h"
_Static_assert(sizeof(D3DCAPS9)==PW_D3D9_CAP_WORDS*4,"complete device caps");
_Static_assert(sizeof(float)==4 && sizeof(int)==4 && sizeof(BOOL)==4,"D3D9 scalar ABI");
static void floating(uint32_t *out,const float *value){memcpy(out,value,4);}
static void color(uint32_t *out,const D3DCOLORVALUE *v)
{floating(out,&v->r);floating(out+1,&v->g);floating(out+2,&v->b);floating(out+3,&v->a);}
int pw_d3d9_native_getter_dispatch(IDirect3DDevice9 *device,
        const struct pw_d3d9_getter_request *request,struct pw_d3d9_getter_reply *out)
{
    struct pw_d3d9_getter_request q;struct pw_d3d9_getter_reply r={0};
    union {DWORD word;UINT integer;D3DDISPLAYMODE mode;D3DRASTER_STATUS raster;
        D3DMATRIX matrix;D3DVIEWPORT9 viewport;D3DMATERIAL9 material;D3DLIGHT9 light;
        D3DCAPS9 caps;D3DCLIPSTATUS9 clip;RECT rect;PALETTEENTRY palette[256];
        float f[1024];int integers[1024];BOOL booleans[1024];} n={0};
    size_t bytes;unsigned i;int status;HRESULT hr=S_OK;uint32_t *d=r.data.words;
    if(!device || !request || !out)return PW_D3D9_GETTER_INVALID;
    q=*request;if((status=pw_d3d9_getter_bytes(&q,&bytes)))return status;
#define CALL(name,...) IDirect3DDevice9_##name(device, ##__VA_ARGS__)
    switch(q.method){
    case 4:n.integer=CALL(GetAvailableTextureMem);break;
    case 7:hr=CALL(GetDeviceCaps,&n.caps);break;
    case 8:hr=CALL(GetDisplayMode,q.args[0],&n.mode);break;
    case 15:n.integer=CALL(GetNumberOfSwapChains);break;
    case 19:hr=CALL(GetRasterStatus,q.args[0],&n.raster);break;
    case 45:hr=CALL(GetTransform,q.args[0],&n.matrix);break;
    case 48:hr=CALL(GetViewport,&n.viewport);break;
    case 50:hr=CALL(GetMaterial,&n.material);break;
    case 52:hr=CALL(GetLight,q.args[0],&n.light);break;
    case 54:hr=CALL(GetLightEnable,q.args[0],&n.booleans[0]);break;
    case 56:hr=CALL(GetClipPlane,q.args[0],n.f);break;
    case 58:hr=CALL(GetRenderState,q.args[0],&n.word);break;
    case 63:hr=CALL(GetClipStatus,&n.clip);break;
    case 66:hr=CALL(GetTextureStageState,q.args[0],q.args[1],&n.word);break;
    case 68:hr=CALL(GetSamplerState,q.args[0],q.args[1],&n.word);break;
    case 70:hr=CALL(ValidateDevice,&n.word);break;
    case 72:hr=CALL(GetPaletteEntries,q.args[0],n.palette);break;
    case 74:hr=CALL(GetCurrentTexturePalette,&n.integer);break;
    case 76:hr=CALL(GetScissorRect,&n.rect);break;
    case 78:n.booleans[0]=CALL(GetSoftwareVertexProcessing);break;
    case 80:n.f[0]=CALL(GetNPatchMode);break;
    case 90:hr=CALL(GetFVF,&n.word);break;
    case 95:hr=CALL(GetVertexShaderConstantF,q.args[0],n.f,q.args[1]);break;
    case 97:hr=CALL(GetVertexShaderConstantI,q.args[0],n.integers,q.args[1]);break;
    case 99:hr=CALL(GetVertexShaderConstantB,q.args[0],n.booleans,q.args[1]);break;
    case 103:hr=CALL(GetStreamSourceFreq,q.args[0],&n.integer);break;
    case 110:hr=CALL(GetPixelShaderConstantF,q.args[0],n.f,q.args[1]);break;
    case 112:hr=CALL(GetPixelShaderConstantI,q.args[0],n.integers,q.args[1]);break;
    case 114:hr=CALL(GetPixelShaderConstantB,q.args[0],n.booleans,q.args[1]);break;
    default:return PW_D3D9_GETTER_UNSUPPORTED;
    }
#undef CALL
    r.method=q.method;r.hresult=(uint32_t)hr;
    if(FAILED(hr)){*out=r;return PW_D3D9_GETTER_OK;}
    r.bytes=(uint32_t)bytes;
    switch(q.method){
    case 4:case 15:case 74:case 103:d[0]=n.integer;break;
    case 7:
        i=0;
#define COPY_CAP(type,name,native) _Static_assert(sizeof(n.caps.native)==4,"caps field width");memcpy(d+i++,&n.caps.native,4);
        PW_D3D9_CAP_FIELDS(COPY_CAP)
#undef COPY_CAP
        break;
    case 8:d[0]=n.mode.Width;d[1]=n.mode.Height;d[2]=n.mode.RefreshRate;d[3]=n.mode.Format;break;
    case 19:d[0]=(uint32_t)n.raster.InVBlank;d[1]=n.raster.ScanLine;break;
    case 45:for(i=0;i<16;i++)floating(d+i,&n.matrix.m[i/4][i%4]);break;
    case 48:d[0]=n.viewport.X;d[1]=n.viewport.Y;d[2]=n.viewport.Width;d[3]=n.viewport.Height;
        floating(d+4,&n.viewport.MinZ);floating(d+5,&n.viewport.MaxZ);break;
    case 50:color(d,&n.material.Diffuse);color(d+4,&n.material.Ambient);color(d+8,&n.material.Specular);color(d+12,&n.material.Emissive);floating(d+16,&n.material.Power);break;
    case 52:d[0]=n.light.Type;color(d+1,&n.light.Diffuse);color(d+5,&n.light.Specular);color(d+9,&n.light.Ambient);
        floating(d+13,&n.light.Position.x);floating(d+14,&n.light.Position.y);floating(d+15,&n.light.Position.z);
        floating(d+16,&n.light.Direction.x);floating(d+17,&n.light.Direction.y);floating(d+18,&n.light.Direction.z);
        floating(d+19,&n.light.Range);floating(d+20,&n.light.Falloff);floating(d+21,&n.light.Attenuation0);
        floating(d+22,&n.light.Attenuation1);floating(d+23,&n.light.Attenuation2);floating(d+24,&n.light.Theta);floating(d+25,&n.light.Phi);break;
    case 54:case 78:d[0]=(uint32_t)n.booleans[0];break;
    case 58:case 66:case 68:case 70:case 90:d[0]=n.word;break;
    case 63:d[0]=n.clip.ClipUnion;d[1]=n.clip.ClipIntersection;break;
    case 72:for(i=0;i<256;i++){r.data.bytes[4*i]=n.palette[i].peRed;r.data.bytes[4*i+1]=n.palette[i].peGreen;
        r.data.bytes[4*i+2]=n.palette[i].peBlue;r.data.bytes[4*i+3]=n.palette[i].peFlags;}break;
    case 76:d[0]=(uint32_t)n.rect.left;d[1]=(uint32_t)n.rect.top;d[2]=(uint32_t)n.rect.right;d[3]=(uint32_t)n.rect.bottom;break;
    case 56:case 80:case 95:case 110:for(i=0;i<bytes/4;i++)floating(d+i,&n.f[i]);break;
    case 97:case 112:for(i=0;i<bytes/4;i++)memcpy(d+i,&n.integers[i],4);break;
    case 99:case 114:for(i=0;i<bytes/4;i++)d[i]=(uint32_t)n.booleans[i];break;
    }
    *out=r;return PW_D3D9_GETTER_OK;
}
