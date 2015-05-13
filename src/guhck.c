#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <zlib.h>
#include <png.h>
#include <chck/buffer/buffer.h>

struct ccs_color {
   uint8_t r, g, b, a;
};

struct ccs_palette {
   uint32_t id;
   uint32_t num_colors;
   const struct ccs_color *colors;
};

struct ccs_image {
   uint32_t id;
   uint32_t pid;
   // 6 bytes ???
   uint32_t width, height;
   // 10 bytes ???
   uint32_t num_palettes;
   const struct ccs_palette *palettes;
   const uint8_t *indices;
};

struct ccs_vec3f {
   float x, y, z;
};

struct ccs_vec2f {
   float x, y;
};

struct ccs_tri3u {
   uint32_t v[3];
};

struct ccs_mesh {
   uint32_t id;
   uint32_t mid;
   uint32_t num_triangles;
   uint32_t num_vertices;
   uint32_t *indices;
   struct ccs_vec3f *vertices;
   struct ccs_vec2f *coords;
};

struct ccs_data {
   const char *name;
   // 24 bytes ???
   uint32_t num_files;
   uint32_t num_objects;
   // 32 bytes ???
   const char **files;
   const char **objects;
   // 8 bytes ???
   // { read until fileType != 0xcccc0005
   //    uint32_t fileType;
   //    uint32_t chunkSize;
   //    void *data;
   // }
   // 12 bytes ???

   uint32_t num_images;
   const struct ccs_image *images;
   uint32_t num_meshes;
   const struct ccs_mesh *meshes;
};

static bool
write_image(const struct ccs_image *image, uint32_t p, const char *path)
{
   assert(image && path);

   FILE *f;
   if (!(f = fopen(path, "wb")))
      return false;

   uint8_t *data;
   const size_t size = image->width * image->height * 4; // RGBA 8bpp
   if (!size || !(data = calloc(1, size)))
      return false;

   // write RGBA from palette
   {
      const struct ccs_palette *palette = &image->palettes[p];
      for (uint32_t i = 0; i < image->height * image->width; ++i) {
         uint8_t index = image->indices[i];
         if (index >= palette->num_colors) {
            printf("-!- Index not in palette: %u, %u\n", index, palette->num_colors);
            continue;
         }
         data[i * 4 + 0] = palette->colors[index].r;
         data[i * 4 + 1] = palette->colors[index].g;
         data[i * 4 + 2] = palette->colors[index].b;
         data[i * 4 + 3] = palette->colors[index].a;
      }
   }

   // invert
   {
      for (uint32_t i = 0; i * 2 < image->height; ++i) {
         uint32_t index1 = i * image->width * 4;
         uint32_t index2 = (image->height - 1 - i) * image->width * 4;
         for (uint32_t i2 = image->width * 4; i2 > 0; --i2) {
            unsigned char temp = data[index1];
            data[index1] = data[index2];
            data[index2] = temp;
            ++index1; ++index2;
         }
      }
   }

   // write png
   {
      png_structp png;
      if (!(png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL)))
         return false;

      png_infop info;
      if (!(info = png_create_info_struct(png)))
         return false;

      if (setjmp(png_jmpbuf(png)))
         return false;

      png_set_IHDR(png, info, image->width, image->height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

      png_byte **rows;
      if (!(rows = png_malloc(png, image->height * sizeof(png_byte*))))
         return false;

      for (uint32_t y = 0; y < image->height; ++y) {
         png_byte *row = png_malloc(png, image->width * 4);
         rows[y] = row;
         for (uint32_t x = 0; x < image->width; ++x) {
            uint32_t i = image->width * y + x;
            *row++ = data[i * 4 + 0];
            *row++ = data[i * 4 + 1];
            *row++ = data[i * 4 + 2];
            *row++ = data[i * 4 + 3];
         }
      }

      png_init_io(png, f);
      png_set_rows(png, info, rows);
      png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);

      for (uint32_t y = 0; y < image->height; ++y)
         png_free(png, rows[y]);

      png_free(png, rows);
      png_destroy_write_struct(&png, &info);
   }

   free(data);
   fclose(f);
   return true;
}

static void
resolve_tristrip(struct ccs_tri3u *faces, uint32_t *f, uint32_t start, uint32_t size, uint32_t type)
{
   assert(faces && f);

   for (uint32_t i = 0; i < size - 2; ++i) {
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

static bool
write_mesh(const struct ccs_mesh *mesh, const char *texture, const char *name, const char *path)
{
   assert(mesh && texture && name && path);

   FILE *f;
   if (!(f = fopen(path, "w")))
      return false;

   fprintf(f, "# guccs (G.U Extractor)\r\n");
   fprintf(f, "# mesh: %s\r\n\r\n", name);
   fprintf(f, "g %s\r\n", name);
   fprintf(f, "usemtl texture\r\n");

   // vertices
   {
      for (uint32_t i = 0; i < mesh->num_vertices; ++i)
         fprintf(f, "v %f %f %f\r\n", mesh->vertices[i].x, mesh->vertices[i].y, mesh->vertices[i].z);
   }

   // coords
   {
      for (uint32_t i = 0; i < mesh->num_vertices; ++i)
         fprintf(f, "vt %f %f\r\n", mesh->coords[i].x, mesh->coords[i].y);
   }

   // faces
   {
      struct ccs_tri3u *faces;
      if (!(faces = calloc(mesh->num_triangles, sizeof(struct ccs_tri3u))))
         return false;

      for (uint32_t ff = 0, i = 0, size = 0, started = 0; i < mesh->num_vertices; ++i) {
         if (started && mesh->indices[i] == 0) {
            ++size;
         } else if (started && mesh->indices[i] != 0) {
            started = 0;
            resolve_tristrip(faces, &ff, i - size, size, mesh->indices[i - size]);
            size = 0;
         }

         if (mesh->indices[i] != 0 && !started) {
            started = 1;
            size += 2;
            ++i;
         }

         if (i == mesh->num_vertices - 1)
            resolve_tristrip(faces, &ff, (i - size) + 1, size, mesh->indices[(i - size) + 1]);
      }

      for (uint32_t i = 0; i < mesh->num_triangles; ++i) {
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
   snprintf(mtl, sizeof(mtl) - 1, "%s.mtl", name);

   if (!(f = fopen(mtl, "w")))
      return false;

   // write material
   {
      fprintf(f, "# guccs (G.U Extractor)\r\n");
      fprintf(f, "# mesh: %s\r\n\r\n", name);
      fprintf(f, "newmtl texture\r\n");
      fprintf(f, "map_Kd %s\r\n", texture);
   }

   fclose(f);
   return true;
}

static bool
read_image(struct chck_buffer *buffer, struct ccs_image *image)
{
   assert(buffer && image);

   chck_buffer_read_int(&image->id, sizeof(image->id), buffer); // ID?
   chck_buffer_read_int(&image->pid, sizeof(image->pid), buffer); // Paletted ID?

   // our IDs start from zero
   image->id -= 1;
   image->pid -= 1;

#if 1
   uint8_t type;
   {
      union {
         uint32_t tmp32;
         uint8_t tmp8;
      } u;
      chck_buffer_read_int(&u.tmp32, sizeof(u.tmp32), buffer); // ID?
      printf("1: %u\n", u.tmp32);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("2: %u\n", u.tmp8);
      chck_buffer_read_int(&type, sizeof(type), buffer); // ???
      printf("3: %u\n", type);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("4: %u\n", u.tmp8);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("5: %u\n", u.tmp8);
   }
#else
   chck_buffer_seek(buffer, 8, SEEK_CUR); // ???
#endif

   uint8_t exponent;
   chck_buffer_read_int(&exponent, sizeof(exponent), buffer);
   image->width = (uint32_t)pow(2.0, exponent);
   chck_buffer_read_int(&exponent, sizeof(exponent), buffer);
   image->height = (uint32_t)pow(2.0, exponent);

#if 0
   {
      union {
         uint8_t tmp8;
      };
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("1: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("2: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("3: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // channels?
      printf("4: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("5: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("6: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("7: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("8: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("9: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(u.tmp8), buffer); // ???
      printf("10: %u\n", tmp);
   }
#else
   chck_buffer_seek(buffer, 10, SEEK_CUR); // ???
#endif

   uint8_t *indices;
   const size_t size = image->width * image->height;
   if (!size || !(indices = calloc(1, size)))
      return false;

   if (type == 19) {
      // 32bit palette
      chck_buffer_read(indices, size, 1, buffer);
   } else if (type == 20) {
      // 16bit palette
      for (uint32_t i = 0, index = 0; i < size; ++i) {
         chck_buffer_read_int(&index, sizeof(index), buffer);
         indices[i++] = index % 16;
         indices[i] = index / 16;
         if (indices[i] == 16 || indices[i - 1] == 16) {
            printf("(%u, %u) %u\n", indices[i - 1], indices[i], index);
            return false;
         }
      }
   } else {
      printf("-!- unknown palette\n");
   }

   image->indices = (const uint8_t*)indices;
   return true;
}

static bool
read_palette(struct chck_buffer *buffer, struct ccs_palette *palette, size_t size)
{
   assert(buffer && palette);

   struct ccs_color *colors;
   const size_t num_colors = (size - 20) / 4;
   if (!num_colors || !(colors = calloc(num_colors, sizeof(struct ccs_color))))
      return false;

   chck_buffer_read_int(&palette->id, sizeof(palette->id), buffer); // ID?
   chck_buffer_seek(buffer, 16, SEEK_CUR); // ???

   // our IDs start from zero
   palette->id -= 1;

   for (uint32_t i = 0; i < num_colors; ++i) {
      chck_buffer_read_int(&colors[i].r, sizeof(colors[i].r), buffer);
      chck_buffer_read_int(&colors[i].g, sizeof(colors[i].g), buffer);
      chck_buffer_read_int(&colors[i].b, sizeof(colors[i].b), buffer);
      chck_buffer_read_int(&colors[i].a, sizeof(colors[i].a), buffer);
      if (colors[i].a <= 128) colors[i].a = (colors[i].a * 255) / 128;
   }

   palette->num_colors = num_colors;
   palette->colors = colors;
   return true;
}

static bool
read_mesh(struct chck_buffer *buffer, struct ccs_mesh *mesh)
{
   assert(buffer && mesh);

   chck_buffer_read_int(&mesh->id, sizeof(mesh->id), buffer); // ID?
#if 0
   {
      union {
         uint32_t tmp32;
      };
      chck_buffer_read_int(&tmp, sizeof(u.tmp32), buffer); // ID?
      printf("1. %u\n", tmp);
      chck_buffer_read_int(&tmp, sizeof(u.tmp32), buffer); // ID?
      printf("2. %u\n", tmp);
      chck_buffer_read_int(&tmp, sizeof(u.tmp32), buffer); // ID?
      printf("3. %u\n", tmp);
   }
#else
   chck_buffer_seek(buffer, 12, SEEK_CUR); // ???
#endif

   // our IDs start from zero
   mesh->id -= 1;

   uint32_t num_indices;
   chck_buffer_read_int(&num_indices, sizeof(num_indices), buffer);

   uint32_t unknown;
   chck_buffer_read_int(&unknown, sizeof(unknown), buffer);
   if (unknown == 0x80000000)
      return false;

   uint32_t unknownid;
   chck_buffer_seek(buffer, 4, SEEK_CUR); // ???
   chck_buffer_read_int(&unknownid, sizeof(unknownid), buffer); // Some ID?
   chck_buffer_read_int(&mesh->mid, sizeof(mesh->mid), buffer); // Material ID?

   // our IDs start from zero
   unknownid -= 1;
   mesh->mid -= 1;

   uint32_t num_vertices;
   chck_buffer_read_int(&num_vertices, sizeof(num_vertices), buffer);

   if (num_vertices > 100000) {
      printf("VERTICES: %u\n", num_vertices);
      return false;
   }

   struct ccs_vec3f *vertices;
   if (!num_vertices || !(vertices = calloc(num_vertices, sizeof(struct ccs_vec3f))))
      return false;

   for (uint32_t i = 0; i < num_vertices; ++i) {
      int8_t data[6];
      chck_buffer_read(data, sizeof(data), 1, buffer);
      vertices[i].x = (float)(data[0] & 0xff) / 256.0f + (float)data[1];
      vertices[i].y = (float)(data[2] & 0xff) / 256.0f + (float)data[3];
      vertices[i].z = (float)(data[4] & 0xff) / 256.0f + (float)data[5];
   }

   chck_buffer_seek(buffer, (num_vertices * 6) % 4, SEEK_CUR); // normals / vcolors ?

   uint32_t *indices;
   if (!num_vertices || !(indices = calloc(num_vertices, sizeof(uint32_t))))
      return false;

   uint32_t num_triangles = 0;
   for (uint32_t i = 0; i < num_vertices; ++i) {
      chck_buffer_seek(buffer, 3, SEEK_CUR); // ???
      uint8_t index;
      chck_buffer_read_int(&index, sizeof(index), buffer); // ^ maybe int?
      indices[i] = index;
      num_triangles += (index == 0);
   }

   chck_buffer_seek(buffer, num_vertices * 4, SEEK_CUR); // normals / vcolors ?

   struct ccs_vec2f *coords;
   if (!num_vertices || !(coords = calloc(num_vertices, sizeof(struct ccs_vec2f))))
      return false;

   for (uint32_t i = 0; i < num_vertices; ++i) {
      int8_t data[4];
      chck_buffer_read(data, sizeof(data), 1, buffer);
      coords[i].x = (float)(data[0] & 0xff) / 256.0f + (float)data[1];
      coords[i].y = (float)(data[2] & 0xff) / 256.0f + (float)data[3];
   }

   mesh->num_triangles = num_triangles;
   mesh->num_vertices = num_vertices;
   mesh->indices = indices;
   mesh->vertices = vertices;
   mesh->coords = coords;
   return true;
}

static bool
read_header(struct chck_buffer *buffer)
{
   assert(buffer);
   uint32_t header = 0;
   chck_buffer_read_int(&header, sizeof(header), buffer);
   return (header == 0xcccc0001);
}

static bool
read_contents(struct chck_buffer *buffer, struct ccs_data *data)
{
   assert(buffer && data);

   chck_buffer_read_string_of_type((char **)&data->name, NULL, 4, buffer);
   chck_buffer_seek(buffer, 23, SEEK_CUR); // useless waste
   chck_buffer_seek(buffer, 24, SEEK_CUR); // ???
   chck_buffer_read_int(&data->num_files, sizeof(data->num_files), buffer);
   chck_buffer_read_int(&data->num_objects, sizeof(data->num_objects), buffer);

   printf("%s (%zu)\n", data->name, strlen(data->name));

   // format counts from 1..9, we count from 0..9
   data->num_files -= (data->num_files > 0);
   data->num_objects -= (data->num_objects > 0);

   if (data->num_files > 10000 || data->num_objects > 10000) {
      printf("too many files: %u, %u\n", data->num_files, data->num_objects);
      return false;
   }

   // read file names
   chck_buffer_seek(buffer, 32, SEEK_CUR); // ???
   if (data->num_files) {
      char **strings;
      if (!(strings = calloc(data->num_files, sizeof(char*))))
         return false;

      if (!(data->files = (const char**)strings))
         return false;

      for (uint32_t i = 0; i < data->num_files; ++i) {
         if (!(strings[i] = calloc(1, 33)))
            return false;

         chck_buffer_read(strings[i], 32, 1, buffer);
         const uint32_t len = strlen(strings[i]);
         if (len && len + 1 < 32 && !(strings[i] = realloc(strings[i], len + 1)))
            return false;
      }
   }

   // read object names
   chck_buffer_seek(buffer, 32, SEEK_CUR); // ???
   if (data->num_objects) {
      char **strings;
      if (!data->num_objects || !(strings = calloc(data->num_objects, sizeof(char*))))
            return false;

      data->objects = (const char**)strings;

      for (uint32_t i = 0; i < data->num_objects; ++i) {
         if (!(strings[i] = calloc(1, 33)))
            return false;

         chck_buffer_read(strings[i], 32, 1, buffer);
         const uint32_t len = strlen(strings[i]);
         if (len && len + 1 < 32 && !(strings[i] = realloc(strings[i], len + 1)))
            return false;
      }
   }

   // read data
   {
      uint32_t filetype = 0xcccc0005;
#if 0
      union {
         uint8_t tmp8;
      };
      chck_buffer_read_int(&u.tmp8, sizeof(uint8_t), buffer);
      printf("1: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(uint8_t), buffer);
      printf("2: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(uint8_t), buffer);
      printf("3: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(uint8_t), buffer);
      printf("4: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(uint8_t), buffer);
      printf("5: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(uint8_t), buffer);
      printf("6: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(uint8_t), buffer);
      printf("7: %u\n", tmp);
      chck_buffer_read_int(&u.tmp8, sizeof(uint8_t), buffer);
      printf("8: %u\n", tmp);
#else
      chck_buffer_seek(buffer, 8, SEEK_CUR); // ???
#endif

      uint32_t num_palettes = 0, mem_palettes = 2;
      struct ccs_palette *palettes;
      if (!(palettes = calloc(mem_palettes, sizeof(struct ccs_palette))))
         return false;

      uint32_t num_images = 0, mem_images = 2;
      struct ccs_image *images;
      if (!(images = calloc(mem_images, sizeof(struct ccs_image))))
         return false;

      uint32_t num_meshes = 0, mem_meshes = 2;
      struct ccs_mesh *meshes;
      if (!(meshes = calloc(mem_meshes, sizeof(struct ccs_mesh))))
         return false;

      while (1) {
         chck_buffer_read_int(&filetype, sizeof(filetype), buffer);
         if (filetype == 0x0 || filetype == 0xcccc0005 || filetype == 0xcccc1b00)
            break;
#if 0
         printf("0x%x\n", filetype);
#endif

         uint32_t chunk_size = 0;
         chck_buffer_read_int(&chunk_size, sizeof(chunk_size), buffer);
         if (chunk_size * 4 > buffer->size - (buffer->curpos - buffer->buffer))
            break;

         size_t start_offset = (buffer->curpos - buffer->buffer);

#if 0
         uint32_t id;
         chck_buffer_read_int(&id, sizeof(id), buffer);
         id -= (id > 0);
         printf("0x%x (%u, %u)\n", filetype, id, chunk_size);
         printf("%s\n", data->objects[id]);
         chck_buffer_seek(buffer, start_offset, SEEK_SET);
#endif

         size_t trail = 0;
         switch (filetype) {
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
               if (read_mesh(buffer, &meshes[num_meshes])) {
                  if (++num_meshes >= mem_meshes) {
                     mem_meshes *= 2;
                     if (!(meshes = realloc(meshes, mem_meshes * sizeof(struct ccs_mesh))))
                        return false;
                  }
               }
               break;
            case 0xcccc0900: // CMP
               break;
            case 0xcccc0400: // PALETTE
               if (!read_palette(buffer, &palettes[num_palettes], chunk_size * 4))
                  return false;
               if (++num_palettes >= mem_palettes) {
                  mem_palettes *= 2;
                  if (!(palettes = realloc(palettes, mem_palettes * sizeof(struct ccs_palette))))
                     return false;
               }
               break;
            case 0xcccc0300: // IMAGE
               {
                  trail = 200;
                  if (!read_image(buffer, &images[num_images]))
                     return false;

                  if (!num_palettes) {
                     free(palettes);
                     palettes = NULL;
                  } else if (num_palettes + 1 < mem_palettes) {
                     palettes = realloc(palettes, (num_palettes + 1) * sizeof(struct ccs_palette));
                  }

                  images[num_images].num_palettes = num_palettes;
                  images[num_images].palettes = palettes;

                  if (++num_images >= mem_images) {
                     mem_images *= 2;
                     if (!(images = realloc(images, mem_images * sizeof(struct ccs_image))))
                        return false;
                  }

                  num_palettes = 0;
                  mem_palettes = 2;
                  if (!(palettes = calloc(mem_palettes, sizeof(struct ccs_palette))))
                     return false;
               }
               break;
            default:break;
         }

         chck_buffer_seek(buffer, start_offset, SEEK_SET);
         chck_buffer_seek(buffer, chunk_size * 4 - trail, SEEK_CUR);
      }

      if (palettes)
         free(palettes);

      if (!num_images) {
         free(images);
         images = NULL;
      } else if (num_images + 1 < mem_images) {
         images = realloc(images, (num_images + 1) * sizeof(struct ccs_image));
      }

      data->num_images = num_images;
      data->images = (const struct ccs_image*)images;

      if (!num_meshes) {
         free(meshes);
         meshes = NULL;
      } else if (num_meshes + 1 < mem_meshes) {
         meshes = realloc(meshes, (num_meshes + 1) * sizeof(struct ccs_mesh));
      }
      data->num_meshes = num_meshes;
      data->meshes = (const struct ccs_mesh*)meshes;
   }

   // trailing 12 bytes ???
   return true;
}

int
main(int argc, char **argv)
{
   if (argc < 2) {
      char *base;
      if (!(base = strrchr(argv[0], '/')))
         return EXIT_SUCCESS;

      if (base) base++; else base = argv[0];
      fprintf(stderr, "usage: %s <file>\n", base);
      return EXIT_SUCCESS;
   }

   // open and decompress file if needed
   gzFile gf;
   if (!(gf = gzopen(argv[1], "rb"))) {
      fprintf(stderr, "cannot open file: %s\n", argv[1]);
      return EXIT_FAILURE;
   }

   size_t size = 4096000;
   struct chck_buffer buffer;
   if (!chck_buffer(&buffer, size, CHCK_ENDIANESS_LITTLE)) {
      fprintf(stderr, "not enough memory (%ld bytes)\n", size);
      return EXIT_FAILURE;
   }

   // decompress/read
   {
      size_t read;
      void *buf;
      if (!(buf = malloc(size))) {
         fprintf(stderr, "not enough memory (%ld bytes)\n", size);
         return EXIT_FAILURE;
      }

      while ((read = gzread(gf, buf, size)) != 0)
         chck_buffer_write(buf, read, 1, &buffer);
      free(buf);
   }

   chck_buffer_seek(&buffer, 0, SEEK_SET);
   gzclose(gf);

   if (!read_header(&buffer)) {
      fprintf(stderr, "invalid header\n");
      return EXIT_FAILURE;
   }

   struct ccs_data data;
   memset(&data, 0, sizeof(data));
   if (!read_contents(&buffer, &data)) {
      fprintf(stderr, "failed to read contents\n");
      return EXIT_FAILURE;
   }

   printf("  ____  _   _    ____ ____ ____    _______  _______ ____      _    ____ _____\n");
   printf(" / ___|| | | |  / ___/ ___/ ___|  | ____\\ \\/ /_   _|  _ \\    / \\  / ___|_   _|\n");
   printf("| |  _ | | | | | |  | |   \\___ \\  |  _|  \\  /  | | | |_) |  / _ \\| |     | |\n");
   printf("| |_| || |_| | | |__| |___ ___) | | |___ /  \\  | | |  _ <  / ___ \\ |___  | |\n");
   printf(" \\____(_)___/   \\____\\____|____/  |_____/_/\\_\\ |_| |_| \\_\\/_/   \\_\\____| |_|\n");
   printf("\n%s (%s)\n", data.name, argv[1]);

#if 1
   printf("\n--- FILES ---\n");
   for (uint32_t i = 0; i < data.num_files; ++i)
      printf("%u. %s\n", i, data.files[i]);
   printf("\n--- OBJECTS ---\n");
   for (uint32_t i = 0; i < data.num_objects; ++i)
      printf("%u. %s\n", i, data.objects[i]);
#endif
   printf("\n--- MESHES ---\n");
   for (uint32_t i = 0; i < data.num_meshes; ++i) {
      printf("• %s\n", data.objects[data.meshes[i].id]);
      printf("    • %s\n", data.objects[data.meshes[i].mid]);

      char buf[256];
      snprintf(buf, sizeof(buf) - 1, "%s.obj", data.objects[data.meshes[i].id]);
      char buf2[256];
      snprintf(buf2, sizeof(buf2) - 1, "%s.png", data.objects[data.meshes[i].mid + 1]);
      write_mesh(&data.meshes[i], buf2, data.objects[data.meshes[i].id], buf);
   }

   printf("\n--- IMAGES ---\n");
   for (uint32_t i = 0; i < data.num_images; ++i) {
      printf("• %s (%ux%u)\n", data.objects[data.images[i].id], data.images[i].width, data.images[i].height);
      for (uint32_t p = 0; p < data.images[i].num_palettes; ++p) {
         printf("    • %s palette with num colors %u\n",
               data.objects[data.images[i].palettes[p].id],
               data.images[i].palettes[p].num_colors);
      }

      char buf[256];
      snprintf(buf, sizeof(buf) - 1, "%s.png", data.objects[data.images[i].id]);
      write_image(&data.images[i], 0, buf);
   }

   printf("\nFILES: %u OBJECTS: %u\n", data.num_files, data.num_objects);
   return EXIT_SUCCESS;
}

/* vim: set ts=8 sw=3 tw=0 :*/
