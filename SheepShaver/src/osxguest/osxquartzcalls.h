/*
 *  osxquartzcalls.h - Leopard libGL dispatch slots forwarded to host OpenGL
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
	case 1: glAccum((GLenum)GlArgument(cpu, 0), (GLfloat)GlFloat(cpu, 1)); break;
	case 2: glAlphaFunc((GLenum)GlArgument(cpu, 0), (GLclampf)GlFloat(cpu, 1)); break;
	case 4: glArrayElement((GLint)GlArgument(cpu, 0)); break;
	case 5: glBegin((GLenum)GlArgument(cpu, 0)); break;
	case 6: glBindTexture((GLenum)GlArgument(cpu, 0), (GLuint)GlArgument(cpu, 1)); break;
	case 8: glBlendFunc((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1)); break;
	case 9: glCallList((GLuint)GlArgument(cpu, 0)); break;
	case 11: glClear((GLbitfield)GlArgument(cpu, 0)); break;
	case 12: glClearAccum((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4)); break;
	case 13: glClearColor((GLclampf)GlFloat(cpu, 1), (GLclampf)GlFloat(cpu, 2), (GLclampf)GlFloat(cpu, 3), (GLclampf)GlFloat(cpu, 4)); break;
	case 14: glClearDepth((GLclampd)GlFloat(cpu, 1)); break;
	case 15: glClearIndex((GLfloat)GlFloat(cpu, 1)); break;
	case 16: glClearStencil((GLint)GlArgument(cpu, 0)); break;
	case 18: glColor3b((GLbyte)GlArgument(cpu, 0), (GLbyte)GlArgument(cpu, 1), (GLbyte)GlArgument(cpu, 2)); break;
	case 19:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLbyte), sizeof(GLbyte)))
			glColor3bv((const GLbyte *)values);
		break;
	case 20: glColor3d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3)); break;
	case 21:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLdouble), sizeof(GLdouble)))
			glColor3dv((const GLdouble *)values);
		break;
	case 22: glColor3f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3)); break;
	case 23:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLfloat), sizeof(GLfloat)))
			glColor3fv((const GLfloat *)values);
		break;
	case 24: glColor3i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 25:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLint), sizeof(GLint)))
			glColor3iv((const GLint *)values);
		break;
	case 26: glColor3s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2)); break;
	case 27:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLshort), sizeof(GLshort)))
			glColor3sv((const GLshort *)values);
		break;
	case 28: glColor3ub((GLubyte)GlArgument(cpu, 0), (GLubyte)GlArgument(cpu, 1), (GLubyte)GlArgument(cpu, 2)); break;
	case 29:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLubyte), sizeof(GLubyte)))
			glColor3ubv((const GLubyte *)values);
		break;
	case 30: glColor3ui((GLuint)GlArgument(cpu, 0), (GLuint)GlArgument(cpu, 1), (GLuint)GlArgument(cpu, 2)); break;
	case 31:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLuint), sizeof(GLuint)))
			glColor3uiv((const GLuint *)values);
		break;
	case 32: glColor3us((GLushort)GlArgument(cpu, 0), (GLushort)GlArgument(cpu, 1), (GLushort)GlArgument(cpu, 2)); break;
	case 33:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLushort), sizeof(GLushort)))
			glColor3usv((const GLushort *)values);
		break;
	case 34: glColor4b((GLbyte)GlArgument(cpu, 0), (GLbyte)GlArgument(cpu, 1), (GLbyte)GlArgument(cpu, 2), (GLbyte)GlArgument(cpu, 3)); break;
	case 35:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLbyte), sizeof(GLbyte)))
			glColor4bv((const GLbyte *)values);
		break;
	case 36: glColor4d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4)); break;
	case 37:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLdouble), sizeof(GLdouble)))
			glColor4dv((const GLdouble *)values);
		break;
	case 38: glColor4f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4)); break;
	case 39:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLfloat), sizeof(GLfloat)))
			glColor4fv((const GLfloat *)values);
		break;
	case 40: glColor4i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3)); break;
	case 41:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLint), sizeof(GLint)))
			glColor4iv((const GLint *)values);
		break;
	case 42: glColor4s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2), (GLshort)GlArgument(cpu, 3)); break;
	case 43:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLshort), sizeof(GLshort)))
			glColor4sv((const GLshort *)values);
		break;
	case 44: glColor4ub((GLubyte)GlArgument(cpu, 0), (GLubyte)GlArgument(cpu, 1), (GLubyte)GlArgument(cpu, 2), (GLubyte)GlArgument(cpu, 3)); break;
	case 45:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLubyte), sizeof(GLubyte)))
			glColor4ubv((const GLubyte *)values);
		break;
	case 46: glColor4ui((GLuint)GlArgument(cpu, 0), (GLuint)GlArgument(cpu, 1), (GLuint)GlArgument(cpu, 2), (GLuint)GlArgument(cpu, 3)); break;
	case 47:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLuint), sizeof(GLuint)))
			glColor4uiv((const GLuint *)values);
		break;
	case 48: glColor4us((GLushort)GlArgument(cpu, 0), (GLushort)GlArgument(cpu, 1), (GLushort)GlArgument(cpu, 2), (GLushort)GlArgument(cpu, 3)); break;
	case 49:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLushort), sizeof(GLushort)))
			glColor4usv((const GLushort *)values);
		break;
	case 50: glColorMask((GLboolean)GlArgument(cpu, 0), (GLboolean)GlArgument(cpu, 1), (GLboolean)GlArgument(cpu, 2), (GLboolean)GlArgument(cpu, 3)); break;
	case 51: glColorMaterial((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1)); break;
	case 53: glCopyPixels((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLsizei)GlArgument(cpu, 2), (GLsizei)GlArgument(cpu, 3), (GLenum)GlArgument(cpu, 4)); break;
	case 54: glCopyTexImage1D((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLenum)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3), (GLint)GlArgument(cpu, 4), (GLsizei)GlArgument(cpu, 5), (GLint)GlArgument(cpu, 6)); break;
	case 55: glCopyTexImage2D((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLenum)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3), (GLint)GlArgument(cpu, 4), (GLsizei)GlArgument(cpu, 5), (GLsizei)GlArgument(cpu, 6), (GLint)GlArgument(cpu, 7)); break;
	case 56: glCopyTexSubImage1D((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3), (GLint)GlArgument(cpu, 4), (GLsizei)GlArgument(cpu, 5)); break;
	case 57: glCopyTexSubImage2D((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3), (GLint)GlArgument(cpu, 4), (GLint)GlArgument(cpu, 5), (GLsizei)GlArgument(cpu, 6), (GLsizei)GlArgument(cpu, 7)); break;
	case 58: glCullFace((GLenum)GlArgument(cpu, 0)); break;
	case 59: glDeleteLists((GLuint)GlArgument(cpu, 0), (GLsizei)GlArgument(cpu, 1)); break;
	case 61: glDepthFunc((GLenum)GlArgument(cpu, 0)); break;
	case 62: glDepthMask((GLboolean)GlArgument(cpu, 0)); break;
	case 63: glDepthRange((GLclampd)GlFloat(cpu, 1), (GLclampd)GlFloat(cpu, 2)); break;
	case 64: glDisable((GLenum)GlArgument(cpu, 0)); break;
	case 70: glEdgeFlag((GLboolean)GlArgument(cpu, 0)); break;
	case 73: glEnable((GLenum)GlArgument(cpu, 0)); break;
	case 75: glEnd(); break;
	case 76: glEndList(); break;
	case 77: glEvalCoord1d((GLdouble)GlFloat(cpu, 1)); break;
	case 79: glEvalCoord1f((GLfloat)GlFloat(cpu, 1)); break;
	case 81: glEvalCoord2d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2)); break;
	case 83: glEvalCoord2f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2)); break;
	case 85: glEvalMesh1((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 86: glEvalMesh2((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3), (GLint)GlArgument(cpu, 4)); break;
	case 87: glEvalPoint1((GLint)GlArgument(cpu, 0)); break;
	case 88: glEvalPoint2((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1)); break;
	case 92: glFogf((GLenum)GlArgument(cpu, 0), (GLfloat)GlFloat(cpu, 1)); break;
	case 94: glFogi((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1)); break;
	case 96: glFrontFace((GLenum)GlArgument(cpu, 0)); break;
	case 97: glFrustum((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4), (GLdouble)GlFloat(cpu, 5), (GLdouble)GlFloat(cpu, 6)); break;
	case 98: cpu->gpr[3] = glGenLists((GLsizei)GlArgument(cpu, 0)); break;
	case 129: glHint((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1)); break;
	case 130: glIndexMask((GLuint)GlArgument(cpu, 0)); break;
	case 132: glIndexd((GLdouble)GlFloat(cpu, 1)); break;
	case 134: glIndexf((GLfloat)GlFloat(cpu, 1)); break;
	case 136: glIndexi((GLint)GlArgument(cpu, 0)); break;
	case 138: glIndexs((GLshort)GlArgument(cpu, 0)); break;
	case 140: glIndexub((GLubyte)GlArgument(cpu, 0)); break;
	case 142: glInitNames(); break;
	case 145: cpu->gpr[3] = glIsList((GLuint)GlArgument(cpu, 0)); break;
	case 146: cpu->gpr[3] = glIsTexture((GLuint)GlArgument(cpu, 0)); break;
	case 147: glLightModelf((GLenum)GlArgument(cpu, 0), (GLfloat)GlFloat(cpu, 1)); break;
	case 149: glLightModeli((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1)); break;
	case 151: glLightf((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLfloat)GlFloat(cpu, 1)); break;
	case 153: glLighti((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 155: glLineStipple((GLint)GlArgument(cpu, 0), (GLushort)GlArgument(cpu, 1)); break;
	case 156: glLineWidth((GLfloat)GlFloat(cpu, 1)); break;
	case 157: glListBase((GLuint)GlArgument(cpu, 0)); break;
	case 158: glLoadIdentity(); break;
	case 161: glLoadName((GLuint)GlArgument(cpu, 0)); break;
	case 162: glLogicOp((GLenum)GlArgument(cpu, 0)); break;
	case 167: glMapGrid1d((GLint)GlArgument(cpu, 0), (GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2)); break;
	case 168: glMapGrid1f((GLint)GlArgument(cpu, 0), (GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2)); break;
	case 169: glMapGrid2d((GLint)GlArgument(cpu, 0), (GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLint)GlArgument(cpu, 5), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4)); break;
	case 170: glMapGrid2f((GLint)GlArgument(cpu, 0), (GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLint)GlArgument(cpu, 3), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4)); break;
	case 171: glMaterialf((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLfloat)GlFloat(cpu, 1)); break;
	case 173: glMateriali((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 175: glMatrixMode((GLenum)GlArgument(cpu, 0)); break;
	case 178: glNewList((GLuint)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1)); break;
	case 179: glNormal3b((GLbyte)GlArgument(cpu, 0), (GLbyte)GlArgument(cpu, 1), (GLbyte)GlArgument(cpu, 2)); break;
	case 180:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLbyte), sizeof(GLbyte)))
			glNormal3bv((const GLbyte *)values);
		break;
	case 181: glNormal3d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3)); break;
	case 182:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLdouble), sizeof(GLdouble)))
			glNormal3dv((const GLdouble *)values);
		break;
	case 183: glNormal3f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3)); break;
	case 184:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLfloat), sizeof(GLfloat)))
			glNormal3fv((const GLfloat *)values);
		break;
	case 185: glNormal3i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 186:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLint), sizeof(GLint)))
			glNormal3iv((const GLint *)values);
		break;
	case 187: glNormal3s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2)); break;
	case 188:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLshort), sizeof(GLshort)))
			glNormal3sv((const GLshort *)values);
		break;
	case 190: glOrtho((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4), (GLdouble)GlFloat(cpu, 5), (GLdouble)GlFloat(cpu, 6)); break;
	case 191: glPassThrough((GLfloat)GlFloat(cpu, 1)); break;
	case 197: glPixelTransferf((GLenum)GlArgument(cpu, 0), (GLfloat)GlFloat(cpu, 1)); break;
	case 198: glPixelTransferi((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1)); break;
	case 199: glPixelZoom((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2)); break;
	case 200: glPointSize((GLfloat)GlFloat(cpu, 1)); break;
	case 201: glPolygonMode((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1)); break;
	case 202: glPolygonOffset((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2)); break;
	case 204: glPopAttrib(); break;
	case 205: glPopClientAttrib(); break;
	case 206: glPopMatrix(); break;
	case 207: glPopName(); break;
	case 209: glPushAttrib((GLbitfield)GlArgument(cpu, 0)); break;
	case 210: glPushClientAttrib((GLbitfield)GlArgument(cpu, 0)); break;
	case 211: glPushMatrix(); break;
	case 212: glPushName((GLuint)GlArgument(cpu, 0)); break;
	case 213: glRasterPos2d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2)); break;
	case 214:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLdouble), sizeof(GLdouble)))
			glRasterPos2dv((const GLdouble *)values);
		break;
	case 215: glRasterPos2f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2)); break;
	case 216:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLfloat), sizeof(GLfloat)))
			glRasterPos2fv((const GLfloat *)values);
		break;
	case 217: glRasterPos2i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1)); break;
	case 218:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLint), sizeof(GLint)))
			glRasterPos2iv((const GLint *)values);
		break;
	case 219: glRasterPos2s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1)); break;
	case 220:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLshort), sizeof(GLshort)))
			glRasterPos2sv((const GLshort *)values);
		break;
	case 221: glRasterPos3d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3)); break;
	case 222:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLdouble), sizeof(GLdouble)))
			glRasterPos3dv((const GLdouble *)values);
		break;
	case 223: glRasterPos3f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3)); break;
	case 224:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLfloat), sizeof(GLfloat)))
			glRasterPos3fv((const GLfloat *)values);
		break;
	case 225: glRasterPos3i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 226:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLint), sizeof(GLint)))
			glRasterPos3iv((const GLint *)values);
		break;
	case 227: glRasterPos3s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2)); break;
	case 228:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLshort), sizeof(GLshort)))
			glRasterPos3sv((const GLshort *)values);
		break;
	case 229: glRasterPos4d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4)); break;
	case 230:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLdouble), sizeof(GLdouble)))
			glRasterPos4dv((const GLdouble *)values);
		break;
	case 231: glRasterPos4f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4)); break;
	case 232:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLfloat), sizeof(GLfloat)))
			glRasterPos4fv((const GLfloat *)values);
		break;
	case 233: glRasterPos4i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3)); break;
	case 234:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLint), sizeof(GLint)))
			glRasterPos4iv((const GLint *)values);
		break;
	case 235: glRasterPos4s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2), (GLshort)GlArgument(cpu, 3)); break;
	case 236:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLshort), sizeof(GLshort)))
			glRasterPos4sv((const GLshort *)values);
		break;
	case 239: glRectd((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4)); break;
	case 241: glRectf((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4)); break;
	case 243: glRecti((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3)); break;
	case 245: glRects((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2), (GLshort)GlArgument(cpu, 3)); break;
	case 247: cpu->gpr[3] = glRenderMode((GLenum)GlArgument(cpu, 0)); break;
	case 248: glRotated((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4)); break;
	case 249: glRotatef((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4)); break;
	case 250: glScaled((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3)); break;
	case 251: glScalef((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3)); break;
	case 252: glScissor((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLsizei)GlArgument(cpu, 2), (GLsizei)GlArgument(cpu, 3)); break;
	case 254: glShadeModel((GLenum)GlArgument(cpu, 0)); break;
	case 255: glStencilFunc((GLenum)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLuint)GlArgument(cpu, 2)); break;
	case 256: glStencilMask((GLuint)GlArgument(cpu, 0)); break;
	case 257: glStencilOp((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLenum)GlArgument(cpu, 2)); break;
	case 258: glTexCoord1d((GLdouble)GlFloat(cpu, 1)); break;
	case 259:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 1 * sizeof(GLdouble), sizeof(GLdouble)))
			glTexCoord1dv((const GLdouble *)values);
		break;
	case 260: glTexCoord1f((GLfloat)GlFloat(cpu, 1)); break;
	case 261:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 1 * sizeof(GLfloat), sizeof(GLfloat)))
			glTexCoord1fv((const GLfloat *)values);
		break;
	case 262: glTexCoord1i((GLint)GlArgument(cpu, 0)); break;
	case 263:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 1 * sizeof(GLint), sizeof(GLint)))
			glTexCoord1iv((const GLint *)values);
		break;
	case 264: glTexCoord1s((GLshort)GlArgument(cpu, 0)); break;
	case 265:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 1 * sizeof(GLshort), sizeof(GLshort)))
			glTexCoord1sv((const GLshort *)values);
		break;
	case 266: glTexCoord2d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2)); break;
	case 267:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLdouble), sizeof(GLdouble)))
			glTexCoord2dv((const GLdouble *)values);
		break;
	case 268: glTexCoord2f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2)); break;
	case 269:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLfloat), sizeof(GLfloat)))
			glTexCoord2fv((const GLfloat *)values);
		break;
	case 270: glTexCoord2i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1)); break;
	case 271:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLint), sizeof(GLint)))
			glTexCoord2iv((const GLint *)values);
		break;
	case 272: glTexCoord2s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1)); break;
	case 273:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLshort), sizeof(GLshort)))
			glTexCoord2sv((const GLshort *)values);
		break;
	case 274: glTexCoord3d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3)); break;
	case 275:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLdouble), sizeof(GLdouble)))
			glTexCoord3dv((const GLdouble *)values);
		break;
	case 276: glTexCoord3f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3)); break;
	case 277:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLfloat), sizeof(GLfloat)))
			glTexCoord3fv((const GLfloat *)values);
		break;
	case 278: glTexCoord3i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 279:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLint), sizeof(GLint)))
			glTexCoord3iv((const GLint *)values);
		break;
	case 280: glTexCoord3s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2)); break;
	case 281:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLshort), sizeof(GLshort)))
			glTexCoord3sv((const GLshort *)values);
		break;
	case 282: glTexCoord4d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4)); break;
	case 283:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLdouble), sizeof(GLdouble)))
			glTexCoord4dv((const GLdouble *)values);
		break;
	case 284: glTexCoord4f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4)); break;
	case 285:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLfloat), sizeof(GLfloat)))
			glTexCoord4fv((const GLfloat *)values);
		break;
	case 286: glTexCoord4i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3)); break;
	case 287:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLint), sizeof(GLint)))
			glTexCoord4iv((const GLint *)values);
		break;
	case 288: glTexCoord4s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2), (GLshort)GlArgument(cpu, 3)); break;
	case 289:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLshort), sizeof(GLshort)))
			glTexCoord4sv((const GLshort *)values);
		break;
	case 291: glTexEnvf((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLfloat)GlFloat(cpu, 1)); break;
	case 293: glTexEnvi((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 295: glTexGend((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLdouble)GlFloat(cpu, 1)); break;
	case 297: glTexGenf((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLfloat)GlFloat(cpu, 1)); break;
	case 299: glTexGeni((GLenum)GlArgument(cpu, 0), (GLenum)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 309: glTranslated((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3)); break;
	case 310: glTranslatef((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3)); break;
	case 311: glVertex2d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2)); break;
	case 312:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLdouble), sizeof(GLdouble)))
			glVertex2dv((const GLdouble *)values);
		break;
	case 313: glVertex2f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2)); break;
	case 314:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLfloat), sizeof(GLfloat)))
			glVertex2fv((const GLfloat *)values);
		break;
	case 315: glVertex2i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1)); break;
	case 316:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLint), sizeof(GLint)))
			glVertex2iv((const GLint *)values);
		break;
	case 317: glVertex2s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1)); break;
	case 318:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 2 * sizeof(GLshort), sizeof(GLshort)))
			glVertex2sv((const GLshort *)values);
		break;
	case 319: glVertex3d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3)); break;
	case 320:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLdouble), sizeof(GLdouble)))
			glVertex3dv((const GLdouble *)values);
		break;
	case 321: glVertex3f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3)); break;
	case 322:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLfloat), sizeof(GLfloat)))
			glVertex3fv((const GLfloat *)values);
		break;
	case 323: glVertex3i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2)); break;
	case 324:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLint), sizeof(GLint)))
			glVertex3iv((const GLint *)values);
		break;
	case 325: glVertex3s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2)); break;
	case 326:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 3 * sizeof(GLshort), sizeof(GLshort)))
			glVertex3sv((const GLshort *)values);
		break;
	case 327: glVertex4d((GLdouble)GlFloat(cpu, 1), (GLdouble)GlFloat(cpu, 2), (GLdouble)GlFloat(cpu, 3), (GLdouble)GlFloat(cpu, 4)); break;
	case 328:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLdouble), sizeof(GLdouble)))
			glVertex4dv((const GLdouble *)values);
		break;
	case 329: glVertex4f((GLfloat)GlFloat(cpu, 1), (GLfloat)GlFloat(cpu, 2), (GLfloat)GlFloat(cpu, 3), (GLfloat)GlFloat(cpu, 4)); break;
	case 330:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLfloat), sizeof(GLfloat)))
			glVertex4fv((const GLfloat *)values);
		break;
	case 331: glVertex4i((GLint)GlArgument(cpu, 0), (GLint)GlArgument(cpu, 1), (GLint)GlArgument(cpu, 2), (GLint)GlArgument(cpu, 3)); break;
	case 332:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLint), sizeof(GLint)))
			glVertex4iv((const GLint *)values);
		break;
	case 333: glVertex4s((GLshort)GlArgument(cpu, 0), (GLshort)GlArgument(cpu, 1), (GLshort)GlArgument(cpu, 2), (GLshort)GlArgument(cpu, 3)); break;
	case 334:
		if (GlGuestData(cpu, values, GlArgument(cpu, 0), 4 * sizeof(GLshort), sizeof(GLshort)))
			glVertex4sv((const GLshort *)values);
		break;
