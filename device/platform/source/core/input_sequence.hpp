// NxCourier · Copyright (C) 2026 granule256 · GPL-2.0-only
#pragma once
#include "nxc_sdk.hpp"
#include <strings.h>
namespace nxc::input {
struct Step { uint64_t buttons; unsigned milliseconds; };
struct Plan { Step steps[256]; int count=0; unsigned totalMs=0; };
// 先完整验证，避免动作串后半段错误时前半段已经生效。
template<class ParseButtons>
bool parsePlan(const char* spec, long long defaultMs, ParseButtons parseButtons, Plan& out) {
    out.count=0; out.totalMs=0;
    if(!spec || !*spec || defaultMs<0 || defaultMs>5000) return false;
    const char* p=spec;
    while(*p) {
        char part[80]; int n=0;
        while(*p && *p!=';') { if(n>=79) return false; part[n++]=*p++; }
        part[n]='\0'; if(*p==';') ++p;
        if(!n || out.count==256) return false;
        char* colon=std::strchr(part,':'); long long ms=defaultMs;
        if(colon) { *colon='\0'; if(!Args::parseInt(colon+1,&ms)) return false; }
        if(ms<0 || ms>10000 || out.totalMs+ms>60000) return false;
        uint64_t buttons=0;
        if(strcasecmp(part,"WAIT")!=0 && (!parseButtons(part,&buttons) || !buttons)) return false;
        out.steps[out.count++]={buttons,static_cast<unsigned>(ms)};
        out.totalMs+=static_cast<unsigned>(ms);
    }
    return out.count>0;
}
}
