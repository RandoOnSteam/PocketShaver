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
void *OsxGuestEngineCreate(void *share);
void OsxGuestEngineDestroy(void *engine);
void OsxGuestEngineBind(void *engine, OsxCpu *cpu);
void OsxGuestEngineResize(void *engine, OsxCpu *cpu, uint32 width, uint32 height);
void OsxGuestEngineCall(void *engine, OsxCpu *cpu, uint32 sub);
void OsxGuestEngineFlush(void *engine, OsxCpu *cpu);
void OsxGuestEngineReadPixels(void *engine, OsxCpu *cpu, uint32 width, uint32 height, void *pixels);
void OsxGuestEngineHostString(void *engine, OsxCpu *cpu, uint32 name, char *text, uint32 size);
int OsxGuestEngineFlurryCall(void *engine, OsxCpu *cpu, uint32 call);
void OsxGuestEngineBeforePresent(void);
void OsxGuestEngineAfterPresent(void);
unsigned OsxGuestEngineTextureCreate(void *engine, OsxCpu *cpu, uint32 width, uint32 height, int floatpixels);
void OsxGuestEngineTextureDelete(void *engine, OsxCpu *cpu, unsigned texture);
void OsxGuestEngineAttachTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height);
void OsxGuestEngineCopyTexture(void *engine, OsxCpu *cpu, unsigned texture, uint32 width, uint32 height, uint32 internalformat, uint32 texturetarget);
void OsxGuestEngineBlendSeparate(void *engine, OsxCpu *cpu, int separate);
int OsxGuestEngineProgramCall(void *engine, OsxCpu *cpu, uint32 call);
void OsxGuestEngineProgramEnable(void *engine, OsxCpu *cpu, uint32 capability, int enable);
}
#endif
