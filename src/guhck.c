#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include <png.h>
#include "buffer/buffer.h"

typedef struct CCSColor {
   unsigned char r, g, b, a;
} CCSColor;

typedef struct CCSPalette {
   unsigned int id;
   unsigned int numColors;
   const CCSColor *colors;
} CCSPalette;

typedef struct CCSImage {
   unsigned int id;
   unsigned int pid;
   // 6 bytes ???
   unsigned int width, height;
   // 10 bytes ???
   unsigned int numPalettes;
   const CCSPalette *palettes;
   const unsigned char *indices;
} CCSImage;

typedef struct CCSVector3f {
   float x, y, z;
} CCSVector3f;

typedef struct CCSVector2f {
   float x, y;
} CCSVector2f;

typedef struct CCSTriangle {
   unsigned int v[3];
} CCSTriangle;

typedef struct CCSMesh {
   unsigned int id;
   unsigned int mid;
   unsigned int numTriangles;
   unsigned int numVertices;
   unsigned int *indices;
   CCSVector3f *vertices;
   CCSVector2f *coords;
} CCSMesh;

typedef struct CCSData {
   const char *name;
   // 24 bytes ???
   unsigned int numFileNames;
   unsigned int numObjectNames;
   // 32 bytes ???
   const char **fileNames;
   const char **objectNames;
   // 8 bytes ???
   // { read until fileType != 0xcccc0005
   //    unsigned int fileType;
   //    unsigned int chunkSize;
   //    void *data;
   // }
   // 12 bytes ???

   unsigned int numImages;
   const CCSImage *images;
   unsigned int numMeshes;
   const CCSMesh *meshes;
} CCSData;

static int writeImage(const CCSImage *image, unsigned int p, const char *path)
{
   FILE *f = fopen(path, "wb");
   if (!f) return 0;

   const CCSPalette *palette = &image->palettes[p];
   unsigned char *data = calloc(1, image->width * image->height * 4); // RGBA 8bpp
   if (!data) return 0;

   // write RGBA from palette
   {
      unsigned int i = 0;
      for (i = 0; i < image->height * image->width; ++i) {
         unsigned char index = image->indices[i];
         if (index >= palette->numColors) printf("%u, %u\n", index, palette->numColors);
         data[i * 4 + 0] = palette->colors[index].r;
         data[i * 4 + 1] = palette->colors[index].g;
         data[i * 4 + 2] = palette->colors[index].b;
         data[i * 4 + 3] = palette->colors[index].a;
      }
   }

   // invert
   {
      unsigned int i, i2;
      for (i = 0; i*2 < image->height; ++i) {
         unsigned int index1 = i * image->width * 4;
         unsigned int index2 = (image->height - 1 - i) * image->width * 4;
         for (i2 = image->width * 4; i2 > 0; --i2) {
            unsigned char temp = data[index1];
            data[index1] = data[index2];
            data[index2] = temp;
            ++index1; ++index2;
         }
      }
   }

   // write png
   {
      png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
      if (!png) return 0;

      png_infop info = png_create_info_struct(png);
      if (!info) return 0;

      if (setjmp(png_jmpbuf(png))) {
         return 0;
      }

      png_set_IHDR(png, info, image->width, image->height, 8,
            PNG_COLOR_TYPE_RGBA,
            PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT,
            PNG_FILTER_TYPE_DEFAULT);

      png_byte **rows = png_malloc(png, image->height * sizeof(png_byte*));
      if (!rows) return 0;

      unsigned int x, y;
      for (y = 0; y < image->height; ++y) {
         png_byte *row = png_malloc(png, image->width * 4);
         rows[y] = row;
         for (x = 0; x < image->width; ++x) {
            unsigned int i = image->width * y + x;
            *row++ = data[i * 4 + 0];
            *row++ = data[i * 4 + 1];
            *row++ = data[i * 4 + 2];
            *row++ = data[i * 4 + 3];
         }
      }

      png_init_io(png, f);
      png_set_rows(png, info, rows);
      png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);

      for (y = 0; y < image->height; ++y) png_free(png, rows[y]);
      png_free(png, rows);
      png_destroy_write_struct(&png, &info);
   }

   free(data);
   fclose(f);
   return 1;
}

static void resolveTriStrip(CCSTriangle *faces, unsigned int *f, unsigned int start, unsigned int size, unsigned int type)
{
   unsigned int i;
   for (i = 0; i < size - 2; ++i) {
      switch (type) {
         case 1:
            if (i % 2 == 1) {
               faces[*f].v[0] = start + i + 1;
               faces[*f].v[1] = start + i;
               faces[*f].v[2] = start + i + 2;
               *f = *f + 1;
            } else {
               faces[*f].v[0] = start + i;
               faces[*f].v[1] = start + i + 1;
               faces[*f].v[2] = start + i + 2;
               *f = *f + 1;
            }
            break;

         case 2:
            if (i % 2 == 1) {
               faces[*f].v[0] = start + i;
               faces[*f].v[1] = start + i + 1;
               faces[*f].v[2] = start + i + 2;
               *f = *f + 1;
            } else {
               faces[*f].v[0] = start + i + 1;
               faces[*f].v[1] = start + i;
               faces[*f].v[2] = start + i + 2;
               *f = *f + 1;
            }
            break;

         default:
            fprintf(stderr, "failed to resolve tristrip\n");
            abort();
      }
   }
}

static int writeMesh(const CCSMesh *mesh, const char *texture, const char *name, const char *path)
{
   FILE *f = fopen(path, "w");
   if (!f) return 0;

   fprintf(f, "# guccs (G.U Extractor)\r\n");
   fprintf(f, "# mesh: %s\r\n\r\n", name);
   fprintf(f, "g %s\r\n", name);
   fprintf(f, "usemtl texture\r\n");

   // vertices
   {
      unsigned int i;
      for (i = 0; i < mesh->numVertices; ++i) {
         fprintf(f, "v %f %f %f\r\n",
               mesh->vertices[i].x,
               mesh->vertices[i].y,
               mesh->vertices[i].z);
      }
   }

   // coords
   {
      unsigned int i;
      for (i = 0; i < mesh->numVertices; ++i) {
         fprintf(f, "vt %f %f\r\n",
               mesh->coords[i].x,
               mesh->coords[i].y);
      }
   }

   // faces
   {
      CCSTriangle *faces = calloc(mesh->numTriangles, sizeof(CCSTriangle));
      if (!faces) return 0;

      unsigned int ff, i, size = 0, started = 0;
      for (ff = 0, i = 0; i < mesh->numVertices; ++i) {
         if (started && mesh->indices[i] == 0) {
            ++size;
         } else if (started && mesh->indices[i] != 0) {
            started = 0;
            resolveTriStrip(faces, &ff, i - size, size, mesh->indices[i - size]);
            size = 0;
         }

         if (mesh->indices[i] != 0 && !started) {
            started = 1;
            size += 2;
            ++i;
         }

         if (i == mesh->numVertices - 1) {
            resolveTriStrip(faces, &ff, (i - size) + 1, size, mesh->indices[(i - size) + 1]);
         }
      }

      for (i = 0; i < mesh->numTriangles; ++i) {
         fprintf(f, "f %u/%u %u/%u %u/%u\r\n",
               faces[i].v[0] + 1,
               faces[i].v[0] + 1,
               faces[i].v[1] + 1,
               faces[i].v[1] + 1,
               faces[i].v[2] + 1,
               faces[i].v[2] + 1);
      }

      free(faces);
   }

   fclose(f);

   char mtl[256];
   snprintf(mtl, sizeof(mtl)-1, "%s.mtl", name);
   f = fopen(mtl, "w");
   if (!f) return 0;

   // write material
   {
      fprintf(f, "# guccs (G.U Extractor)\r\n");
      fprintf(f, "# mesh: %s\r\n\r\n", name);
      fprintf(f, "newmtl texture\r\n");
      fprintf(f, "map_Kd %s\r\n", texture);
   }

   fclose(f);
   return 1;
}

static int readImage(chckBuffer *buffer, CCSImage *image)
{
   chckBufferReadUInt32(buffer, &image->id); // ID?
   chckBufferReadUInt32(buffer, &image->pid); // Palette ID?

   // our IDs start from zero
   image->id -= 1;
   image->pid -= 1;

#if 1
   unsigned int tmp2;
   chckBufferReadUInt32(buffer, &tmp2); // ???
   printf("1: %u\n", tmp2);
   unsigned char tmp3;
   chckBufferReadUInt8(buffer, &tmp3); // ???
   printf("2: %u\n", tmp3);
   unsigned char type;
   chckBufferReadUInt8(buffer, &type); // ???
   printf("3: %u\n", type);
   chckBufferReadUInt8(buffer, &tmp3); // ???
   printf("4: %u\n", tmp3);
   chckBufferReadUInt8(buffer, &tmp3); // ???
   printf("5: %u\n", tmp3);
#else
   chckBufferSeek(buffer, 8, SEEK_CUR); // ???
#endif

   unsigned char exponent;
   chckBufferReadUInt8(buffer, &exponent);
   image->width = (unsigned int)pow(2.0, exponent);
   chckBufferReadUInt8(buffer, &exponent);
   image->height = (unsigned int)pow(2.0, exponent);

#if 0
   unsigned char tmp;
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("1: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("2: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("3: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // channels?
   printf("4: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("5: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("6: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("7: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("8: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("9: %u\n", tmp);
   chckBufferReadUInt8(buffer, &tmp); // ???
   printf("10: %u\n", tmp);
#else
   chckBufferSeek(buffer, 10, SEEK_CUR); // ???
#endif

   char *indices = calloc(1, image->width * image->height);
   if (!indices) return 0;

   if (type == 19) {
      // 32bit palette
      chckBufferRead(indices, image->width * image->height, 1, buffer);
   } else if (type == 20) {
      // 16bit palette
      unsigned int i;
      unsigned char index = 0;
      for (i = 0; i < image->width * image->height; ++i) {
         chckBufferReadUInt8(buffer, &index);
         indices[i++] = index % 16;
         indices[i] = index / 16;
         if (indices[i] == 16 || indices[i-1] == 16) {
            printf("(%u, %u) %u\n", indices[i-1], indices[i], index);
            return 0;
         }
      }
   } else {
      printf("-!- unknown palette\n");
   }

   image->indices = (const unsigned char*)indices;
   return 1;
}

static int readPalette(chckBuffer *buffer, CCSPalette *palette, size_t size)
{
   size_t numColors = (size - 20) / 4;
   CCSColor *colors = calloc(numColors, sizeof(CCSColor));
   if (!colors) return 0;

   chckBufferReadUInt32(buffer, &palette->id); // ID?
   chckBufferSeek(buffer, 16, SEEK_CUR); // ???

   // our IDs start from zero
   palette->id -= 1;

   unsigned int i = 0;
   for (i = 0; i < numColors; ++i) {
      chckBufferReadUInt8(buffer, &colors[i].r);
      chckBufferReadUInt8(buffer, &colors[i].g);
      chckBufferReadUInt8(buffer, &colors[i].b);
      chckBufferReadUInt8(buffer, &colors[i].a);
      if (colors[i].a <= 128) colors[i].a = (colors[i].a * 255) / 128;
   }

   palette->numColors = numColors;
   palette->colors = (CCSColor*)colors;
   return 1;
}

static int readMesh(chckBuffer *buffer, CCSMesh *mesh)
{
   chckBufferReadUInt32(buffer, &mesh->id); // ID?
#if 0
   unsigned int tmp;
   chckBufferReadUInt32(buffer, &tmp); // ID?
   printf("1. %u\n", tmp);
   chckBufferReadUInt32(buffer, &tmp); // ID?
   printf("2. %u\n", tmp);
   chckBufferReadUInt32(buffer, &tmp); // ID?
   printf("3. %u\n", tmp);
#else
   chckBufferSeek(buffer, 12, SEEK_CUR); // ???
#endif

   // our IDs start from zero
   mesh->id -= 1;

   unsigned int numIndices;
   chckBufferReadUInt32(buffer, &numIndices);

   unsigned int unknown;
   chckBufferReadUInt32(buffer, &unknown);
   if (unknown == 0x80000000) return 0;

   unsigned int unknownid;
   chckBufferSeek(buffer, 4, SEEK_CUR); // ???
   chckBufferReadUInt32(buffer, &unknownid); // Some ID?
   chckBufferReadUInt32(buffer, &mesh->mid); // Material ID?

   // our IDs start from zero
   unknownid -= 1;
   mesh->mid -= 1;

   unsigned int numVertices;
   chckBufferReadUInt32(buffer, &numVertices);

   if (numVertices > 100000) {
      printf("VERTICES: %u\n", numVertices);
      return 0;
   }

   CCSVector3f *vertices = calloc(numVertices, sizeof(CCSVector3f));
   if (!vertices) return 0;

   unsigned int i;
   for (i = 0; i < numVertices; ++i) {
      char data[6];
      chckBufferRead(data, sizeof(data), 1, buffer);
      vertices[i].x = (float)(data[0] & 0xff) / 256.0f + (float)data[1];
      vertices[i].y = (float)(data[2] & 0xff) / 256.0f + (float)data[3];
      vertices[i].z = (float)(data[4] & 0xff) / 256.0f + (float)data[5];
   }

   chckBufferSeek(buffer, (numVertices * 6) % 4, SEEK_CUR); // normals / vcolors ?

   unsigned int *indices = calloc(numVertices, sizeof(unsigned int));
   if (!indices) return 0;

   unsigned int numTriangles = 0;
   for (i = 0; i < numVertices; ++i) {
      chckBufferSeek(buffer, 3, SEEK_CUR); // ???
      unsigned char index;
      chckBufferReadUInt8(buffer, &index); // ^ maybe int?
      indices[i] = index;
      if (index == 0) ++numTriangles;
   }

   chckBufferSeek(buffer, numVertices * 4, SEEK_CUR); // normals / vcolors ?
   CCSVector2f *coords = calloc(numVertices, sizeof(CCSVector2f));
   if (!coords) return 0;

   for (i = 0; i < numVertices; ++i) {
      char data[4];
      chckBufferRead(data, sizeof(data), 1, buffer);
      coords[i].x = (float)(data[0] & 0xff) / 256.0f + (float)data[1];
      coords[i].y = (float)(data[2] & 0xff) / 256.0f + (float)data[3];
   }

   mesh->numTriangles = numTriangles;
   mesh->numVertices = numVertices;
   mesh->indices = indices;
   mesh->vertices = vertices;
   mesh->coords = coords;
   return 1;
}

static int readHeader(chckBuffer *buffer)
{
   unsigned int header = 0;
   chckBufferReadUInt32(buffer, &header);
   return (header == 0xcccc0001);
}

static int readContents(chckBuffer *buffer, CCSData *data)
{
   chckBufferReadString(buffer, 4, (char**)&data->name);
   chckBufferSeek(buffer, 23, SEEK_CUR); // useless waste
   chckBufferSeek(buffer, 24, SEEK_CUR); // ???
   chckBufferReadUInt32(buffer, &data->numFileNames);
   chckBufferReadUInt32(buffer, &data->numObjectNames);

   printf("%s (%zu)\n", data->name, strlen(data->name));

   // format counts from 1..9, we count from 0..9
   if (data->numFileNames > 0) data->numFileNames -= 1;
   if (data->numObjectNames > 0) data->numObjectNames -= 1;

   if (data->numFileNames > 10000 || data->numObjectNames > 10000) {
      printf("too many files: %u, %u\n", data->numFileNames, data->numObjectNames);
      return 0;
   }

   // read file names
   chckBufferSeek(buffer, 32, SEEK_CUR); // ???
   if (data->numFileNames) {
      char **strings = calloc(data->numFileNames, sizeof(char*));
      data->fileNames = (const char**)strings;
      if (!strings) return 0;

      unsigned int i;
      for (i = 0; i < data->numFileNames; ++i) {
         strings[i] = calloc(1, 33);
         chckBufferRead(strings[i], 32, 1, buffer);
         unsigned int len = strlen(strings[i]);
         if (len && len + 1 < 32) {
            strings[i] = realloc(strings[i], len + 1);
            if (!strings[i]) return 0;
         }
      }
   }

   // read object names
   chckBufferSeek(buffer, 32, SEEK_CUR); // ???
   if (data->numObjectNames) {
      char **strings = calloc(data->numObjectNames, sizeof(char*));
      data->objectNames = (const char**)strings;
      if (!strings) return 0;

      unsigned int i;
      for (i = 0; i < data->numObjectNames; ++i) {
         strings[i] = calloc(1, 33);
         chckBufferRead(strings[i], 32, 1, buffer);
         unsigned int len = strlen(strings[i]);
         if (len && len + 1 < 32) {
            strings[i] = realloc(strings[i], len + 1);
            if (!strings[i]) return 0;
         }
      }
   }

   // read data
   {
      unsigned int fileType = 0xcccc0005;
#if 0
      unsigned char tmp;
      chckBufferReadUInt8(buffer, &tmp);
      printf("1: %u\n", tmp);
      chckBufferReadUInt8(buffer, &tmp);
      printf("2: %u\n", tmp);
      chckBufferReadUInt8(buffer, &tmp);
      printf("3: %u\n", tmp);
      chckBufferReadUInt8(buffer, &tmp);
      printf("4: %u\n", tmp);
      chckBufferReadUInt8(buffer, &tmp);
      printf("5: %u\n", tmp);
      chckBufferReadUInt8(buffer, &tmp);
      printf("6: %u\n", tmp);
      chckBufferReadUInt8(buffer, &tmp);
      printf("7: %u\n", tmp);
      chckBufferReadUInt8(buffer, &tmp);
      printf("8: %u\n", tmp);
#else
      chckBufferSeek(buffer, 8, SEEK_CUR); // ???
#endif

      unsigned int numPalettes = 0;
      unsigned int memPalettes = 2;
      CCSPalette *palettes = calloc(memPalettes, sizeof(CCSPalette));
      if (!palettes) return 0;

      unsigned int numImages = 0;
      unsigned int memImages = 2;
      CCSImage *images = calloc(memImages, sizeof(CCSImage));
      if (!images) return 0;

      unsigned int numMeshes = 0;
      unsigned int memMeshes = 2;
      CCSMesh *meshes = calloc(memMeshes, sizeof(CCSImage));
      if (!meshes) return 0;


      while (1) {
         unsigned int chunkSize = 0;
         size_t startOffset, trail = 0;
         chckBufferReadUInt32(buffer, &fileType);
         if (fileType == 0x0 || fileType == 0xcccc0005 || fileType == 0xcccc1b00) break;
#if 0
         printf("0x%x\n", fileType);
#endif

         chckBufferReadUInt32(buffer, &chunkSize);
         if (chunkSize * 4 > chckBufferGetSize(buffer) - chckBufferGetOffset(buffer)) break;
         startOffset = chckBufferGetOffset(buffer);

#if 0
         unsigned int id;
         chckBufferReadUInt32(buffer, &id);
         if (id > 0) id -= 1;
         printf("0x%x (%u, %u)\n", fileType, id, chunkSize);
         printf("%s\n", data->objectNames[id]);
         chckBufferSeek(buffer, startOffset, SEEK_SET);
#endif

         switch (fileType) {
            case 0xcccc2400: // BIN
               // STRING
               break;
            case 0xcccc0100: // OBJECT
            case 0Xcccc0a00:
            case 0Xcccc2000:
               break;
            case 0xcccc0200: // MATERIAL
               break;
            case 0xcccc0700: // ANIMATION
               break;
            case 0xcccc0800: // MESH
               if (readMesh(buffer, &meshes[numMeshes])) {
                  if (++numMeshes >= memMeshes) {
                     memMeshes *= 2;
                     meshes = realloc(meshes, memMeshes * sizeof(CCSMesh));
                     if (!meshes) return 0;
                  }
               }
               break;
            case 0xcccc0900: // CMP
               break;
            case 0xcccc0400: // PALETTE
               if (!readPalette(buffer, &palettes[numPalettes], chunkSize * 4)) return 0;
               if (++numPalettes >= memPalettes) {
                  memPalettes *= 2;
                  palettes = realloc(palettes, memPalettes * sizeof(CCSPalette));
                  if (!palettes) return 0;
               }
               break;
            case 0xcccc0300: // IMAGE
               {
                  trail = 200;
                  if (!readImage(buffer, &images[numImages])) return 0;

                  if (!numPalettes) {
                     free(palettes);
                     palettes = NULL;
                  } else if (numPalettes + 1 < memPalettes) {
                     palettes = realloc(palettes, (numPalettes + 1) * sizeof(CCSPalette));
                  }
                  images[numImages].numPalettes = numPalettes;
                  images[numImages].palettes = palettes;

                  if (++numImages >= memImages) {
                     memImages *= 2;
                     images = realloc(images, memImages * sizeof(CCSImage));
                     if (!images) return 0;
                  }

                  numPalettes = 0;
                  memPalettes = 2;
                  palettes = calloc(memPalettes, sizeof(CCSPalette));
                  if (!palettes) return 0;
               }
               break;
            default:break;
         }

         chckBufferSeek(buffer, startOffset, SEEK_SET);
         chckBufferSeek(buffer, chunkSize * 4 - trail, SEEK_CUR);
      }

      if (palettes) free(palettes);

      if (!numImages) {
         free(images);
         images = NULL;
      } else if (numImages + 1 < memImages) {
         images = realloc(images, (numImages + 1) * sizeof(CCSImage));
      }
      data->numImages = numImages;
      data->images = (const CCSImage*)images;

      if (!numMeshes) {
         free(meshes);
         meshes = NULL;
      } else if (numMeshes + 1 < memMeshes) {
         meshes = realloc(meshes, (numMeshes + 1) * sizeof(CCSMesh));
      }
      data->numMeshes = numMeshes;
      data->meshes = (const CCSMesh*)meshes;
   }

   // trailing 12 bytes ???
   return 1;
}

int main(int argc, char **argv)
{
   if (argc < 2) {
      char *base = strrchr(argv[0], '/');
      if (base) base++; else base = argv[0];
      fprintf(stderr, "usage: %s <file>\n", base);
      return EXIT_SUCCESS;
   }

   // open and decompress file if needed
   gzFile gf = gzopen(argv[1], "rb");
   if (!gf) {
      fprintf(stderr, "cannot open file: %s\n", argv[1]);
      return EXIT_FAILURE;
   }

   size_t size = 4096000;
   chckBuffer *buffer = chckBufferNew(size, CHCK_BUFFER_ENDIAN_LITTLE);
   if (!buffer) {
      fprintf(stderr, "not enough memory (%ld bytes)\n", size);
      return EXIT_FAILURE;
   }

   // decompress/read
   {
      size_t read;
      void *buf = malloc(size);
      if (!buf) {
         fprintf(stderr, "not enough memory (%ld bytes)\n", size);
         return EXIT_FAILURE;
      }
      while ((read = gzread(gf, buf, size)) != 0)
         chckBufferWrite(buf, read, 1, buffer);
      free(buf);
   }

   chckBufferSeek(buffer, 0, SEEK_SET);
   gzclose(gf);

   if (!readHeader(buffer)) {
      fprintf(stderr, "invalid header\n");
      return EXIT_FAILURE;
   }

   CCSData data;
   memset(&data, 0, sizeof(data));
   if (!readContents(buffer, &data)) {
      fprintf(stderr, "failed to read contents\n");
      return EXIT_FAILURE;
   }

   printf("  ____  _   _    ____ ____ ____    _______  _______ ____      _    ____ _____\n");
   printf(" / ___|| | | |  / ___/ ___/ ___|  | ____\\ \\/ /_   _|  _ \\    / \\  / ___|_   _|\n");
   printf("| |  _ | | | | | |  | |   \\___ \\  |  _|  \\  /  | | | |_) |  / _ \\| |     | |\n");
   printf("| |_| || |_| | | |__| |___ ___) | | |___ /  \\  | | |  _ <  / ___ \\ |___  | |\n");
   printf(" \\____(_)___/   \\____\\____|____/  |_____/_/\\_\\ |_| |_| \\_\\/_/   \\_\\____| |_|\n");
   printf("\n%s (%s)\n", data.name, argv[1]);

   unsigned int i;
#if 1
   printf("\n--- FILES ---\n");
   for (i = 0; i < data.numFileNames; ++i) printf("%u. %s\n", i, data.fileNames[i]);
   printf("\n--- OBJECTS ---\n");
   for (i = 0; i < data.numObjectNames; ++i) printf("%u. %s\n", i, data.objectNames[i]);
#endif
   printf("\n--- MESHES ---\n");
   for (i = 0; i < data.numMeshes; ++i) {
      printf("• %s\n", data.objectNames[data.meshes[i].id]);
      printf("    • %s\n", data.objectNames[data.meshes[i].mid]);

      char buf[256];
      snprintf(buf, sizeof(buf)-1, "%s.obj", data.objectNames[data.meshes[i].id]);
      char buf2[256];
      snprintf(buf2, sizeof(buf2)-1, "%s.png", data.objectNames[data.meshes[i].mid+1]);
      writeMesh(&data.meshes[i], buf2, data.objectNames[data.meshes[i].id], buf);
   }
   printf("\n--- IMAGES ---\n");
   for (i = 0; i < data.numImages; ++i) {
      printf("• %s (%ux%u)\n",
            data.objectNames[data.images[i].id],
            data.images[i].width, data.images[i].height);
      unsigned int p;
      for (p = 0; p < data.images[i].numPalettes; ++p) {
         printf("    • %s palette with num colors %u\n",
               data.objectNames[data.images[i].palettes[p].id],
               data.images[i].palettes[p].numColors);
      }

      char buf[256];
      snprintf(buf, sizeof(buf)-1, "%s.png", data.objectNames[data.images[i].id]);
      writeImage(&data.images[i], 0, buf);
   }
   printf("\nFILES: %u OBJECTS: %u\n", data.numFileNames, data.numObjectNames);

   return EXIT_SUCCESS;
}

/* vim: set ts=8 sw=3 tw=0 :*/
