/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_profile_catalog.h"
#include <assert.h>
#include <string.h>

static int parse(const char *text,PwProfileCatalog *catalog)
{return pw_profile_catalog_parse((const uint8_t *)text,strlen(text),catalog);}

int main(void)
{
    PwProfileCatalog c;
    assert(parse("# package profiles\r\npinball.profile\r\n\n  ; spare\n paint_2-x.profile \t\n",&c)==PW_OK);
    assert(c.count==2 && !strcmp(c.names[0],"pinball.profile") &&
           !strcmp(c.names[1],"paint_2-x.profile"));
    assert(parse("",&c)==PW_OK && !c.count);
    assert(pw_profile_catalog_parse(NULL,0,&c)==PW_OK && !c.count);
    /* A final line without a newline is still an entry. */
    assert(parse("a.profile\nb.profile",&c)==PW_OK && c.count==2 && !strcmp(c.names[1],"b.profile"));
    /* Refusals leave the previous catalogue untouched. */
    const char *bad[]={"../pinball.profile","dir/pinball.profile","Pinball.profile",
        "pinball.txt",".profile","pinball.profile.profile\npinball.profile.profile",
        "pin ball.profile","a.profile\na.profile"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        PwProfileCatalog kept=c;
        assert(parse(bad[i],&c)==PW_ERR_MALFORMED && !memcmp(&kept,&c,sizeof(c)));
    }
    char long_name[80];memset(long_name,'a',sizeof(long_name));
    memcpy(long_name+sizeof(long_name)-9,".profile",9);
    assert(parse(long_name,&c)==PW_ERR_MALFORMED);
    char many[17*16+1];many[0]=0;
    for(unsigned i=0;i<17;i++){char line[16];line[0]=(char)('a'+i);memcpy(line+1,".profile\n",10);strcat(many,line);}
    assert(parse(many,&c)==PW_ERR_LIMIT);
    many[16*10]=0;assert(parse(many,&c)==PW_OK && c.count==16);
    assert(pw_profile_catalog_parse(NULL,1,&c)==PW_ERR_PRECONDITION);
    assert(pw_profile_catalog_parse((const uint8_t *)"",0,NULL)==PW_ERR_PRECONDITION);

    PwAppProfile p;memset(&p,0,sizeof(p));
    strcpy(p.runtime,"prospero-win-direct");p.architecture=PW_APP_ARCH_PE32;p.graphics=PW_APP_GRAPHICS_GDI;
    assert(pw_profile_catalog_launchable(&p));
    strcpy(p.runtime,"wine-wow64");assert(!pw_profile_catalog_launchable(&p));
    strcpy(p.runtime,"prospero-win-direct");p.architecture=PW_APP_ARCH_PE64;
    assert(!pw_profile_catalog_launchable(&p));
    p.architecture=PW_APP_ARCH_PE32;p.graphics=PW_APP_GRAPHICS_DXVK;
    assert(!pw_profile_catalog_launchable(&p));
    assert(!pw_profile_catalog_launchable(NULL));
    return 0;
}
