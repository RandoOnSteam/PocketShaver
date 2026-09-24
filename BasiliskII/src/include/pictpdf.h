/*
 *  pictpdf.h - Converts a macintosh PICT to a PDF
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

#ifndef PICTPDF_H
#define PICTPDF_H

#ifdef __cplusplus
extern "C" {
#endif

struct pdf_doc;

typedef struct PICTPDFMAP
{
	double scalex;
	double scaley;
	double left;
	double top;
	double pageheight;
} PICTPDFMAP;

int PictPdfRenderPage(struct pdf_doc* pdf, const unsigned char* picture, long picturesize, const PICTPDFMAP* map);

#ifdef __cplusplus
}
#endif

#endif
