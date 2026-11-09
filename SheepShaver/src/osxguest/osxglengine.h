/*
 *  osxglengine.h - gfxaccel GL engine built for Mac OS X guest address spaces
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#ifndef OSXGLENGINE_H
#define OSXGLENGINE_H
namespace OsxGl {
void *EngineCreate(void);
void EngineDestroy(void *engine);
void EngineBind(void *engine, OsxCpu *cpu);
void EngineResize(void *engine, OsxCpu *cpu, uint32 width, uint32 height);
void EngineCall(void *engine, OsxCpu *cpu, uint32 sub);
void EngineFlush(void *engine, OsxCpu *cpu);
void EngineReadPixels(void *engine, OsxCpu *cpu, uint32 width, uint32 height, void *pixels);
void EngineHostString(void *engine, OsxCpu *cpu, uint32 name, char *text, uint32 size);
int EngineFlurryCall(void *engine, OsxCpu *cpu, uint32 call);
void EngineAfterPresent(void);
unsigned EngineTextureCreate(void *engine, OsxCpu *cpu, uint32 width, uint32 height, int floatpixels);
void EngineTextureDelete(void *engine, OsxCpu *cpu, unsigned texture);
void EngineAttachTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height);
void EngineCopyTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height, uint32 internalformat);
void EngineBlendSeparate(void *engine, OsxCpu *cpu, int separate);
int EngineProgramCall(void *engine, OsxCpu *cpu, uint32 call);
void EngineProgramEnable(void *engine, OsxCpu *cpu, uint32 capability, int enable);
}
#endif
