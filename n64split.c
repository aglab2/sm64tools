#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "libmio0.h"
#include "mipsdisasm.h"
#include "n64graphics.h"
#include "utils.h"

// extract opcode from instruction MSB
#define OPCODE(VAL_) ((VAL_) & 0xFC)
#define LUI_REG(VAL_) ((VAL_) & 0x1F)

const char asm_header[] = 
   ".set noat      # allow manual use of $at\n"
   ".set noreorder # don't insert nops after branches\n"
   "\n"
   ".global _start\n"
   "\n"
   "_start:\n";

static int lookup_start(split_section *sections, int section_count, unsigned int address)
{
   int i;
   // TODO: binary search
   for (i = 0; i < section_count; i++) {
      if (sections[i].start == address) {
         return i;
      }
   }
   return -1;
}

static int lookup_end(split_section *sections, int section_count, unsigned int address)
{
   int i;
   // TODO: binary search
   for (i = 0; i < section_count; i++) {
      if (sections[i].end == address) {
         return i;
      }
   }
   return -1;
}

static void print_spaces(FILE *fp, int count)
{
   int i;
   for (i = 0; i < count; i++) {
      fputc(' ', fp);
   }
}

static void write_behavior(FILE *out, unsigned char *data, split_section *sections, int section_count, int s)
{
   unsigned int a;
   unsigned int len;
   split_section *sec;
   (void)section_count;
   sec = &sections[s];
   a = sec->start;
   while (a < sec->end) {
      switch (data[a]) {
         case 0x0C:
         case 0x2A:
         case 0x02:
         case 0x23:
         case 0x14:
         case 0x2F:
         case 0x04:
         case 0x27:
            len = 8;
            break;
         case 0x1C:
         case 0x2B:
         case 0x2C:
         case 0x29:
            len = 12;
            break;
         case 0x30:
            len = 20;
            break;
         default:
            len = 4;
            break;
      }
      fprintf(out, ".byte ");
      fprint_hex_source(out, &data[a], len);
      fprintf(out, "\n");
      a += len;
   }
}

static void write_level(FILE *out, unsigned char *data, split_section *sections, int section_count, int s)
{
   char start_label[128];
   char end_label[128];
   char dst_label[128];
   split_section *sec;
   unsigned int ptr_start;
   unsigned int ptr_end;
   unsigned int dst;
   unsigned int a;
   int indent;
   int i;

   sec = &sections[s];

   a = sec->start;
   while (a < sec->end) {
      // length = 0 ends level script
      if (data[a+1] == 0) {
         break;
      }
      switch (data[a]) {
         case 0x00: // load and jump from ROM into a RAM segment
         case 0x17: // copy uncompressed data from ROM to a RAM segment
         case 0x18: // decompress MIO0 data from ROM and copy it into a RAM segment
         case 0x1A: // decompress MIO0 data from ROM and copy it into a RAM segment (for texture only segments?)
            ptr_start = read_u32_be(&data[a+4]);
            ptr_end = read_u32_be(&data[a+8]);
            i = lookup_start(sections, section_count, ptr_start);
            if (i < 0) {
               sprintf(start_label, "0x%08X", ptr_start);
            } else {
               strcpy(start_label, sections[i].label);
            }
            i = lookup_end(sections, section_count, ptr_end);
            if (i < 0) {
               sprintf(end_label, "0x%08X", ptr_end);
            } else {
               sprintf(end_label, "%s_end", sections[i].label);
            }
            fprintf(out, ".word 0x");
            for (i = 0; i < 4; i++) {
               fprintf(out, "%02X", data[a+i]);
            }
            fprintf(out, ", %s, %s", start_label, end_label);
            for (i = 12; i < data[a+1]; i++) {
               if ((i & 0x3) == 0) {
                  fprintf(out, ", 0x");
               }
               fprintf(out, "%02X", data[a+i]);
            }
            fprintf(out, "\n");
            break;
         case 0x16: // load ASM into RAM
            dst       = read_u32_be(&data[a+0x4]);
            ptr_start = read_u32_be(&data[a+0x8]);
            ptr_end   = read_u32_be(&data[a+0xc]);
            i = lookup_start(sections, section_count, dst);
            if (i < 0) {
               sprintf(dst_label, "0x%08X", dst);
            } else {
               strcpy(dst_label, sections[i].label);
            }
            i = lookup_start(sections, section_count, ptr_start);
            if (i < 0) {
               sprintf(start_label, "0x%08X", ptr_start);
            } else {
               strcpy(start_label, sections[i].label);
            }
            i = lookup_end(sections, section_count, ptr_end);
            if (i < 0) {
               sprintf(end_label, "0x%08X", ptr_end);
            } else {
               sprintf(end_label, "%s_end", sections[i].label);
            }
            fprintf(out, ".word 0x");
            for (i = 0; i < 4; i++) {
               fprintf(out, "%02X", data[a+i]);
            }
            fprintf(out, ", %s, %s, %s\n", dst_label, start_label, end_label);
            break;
         default:
            fprintf(out, ".byte ");
            fprint_hex_source(out, &data[a], data[a+1]);
            fprintf(out, "\n");
            break;
      }
      a += data[a+1];
   }
   // align to next 16-byte boundary
   if (a & 0x0F) {
      fprintf(out, "# begin %s alignment 0x%X\n", sec->label, a);
      fprintf(out, ".byte ");
      fprint_hex_source(out, &data[a], ALIGN(a, 16) - a);
      fprintf(out, "\n");
      a = ALIGN(a, 16);
   }
   fprintf(out, "# begin %s geo 0x%X\n", sec->label, a);
   // remaining is geo layout script
   indent = 0;
   while (a < sec->end) {
      switch (data[a]) {
         case 0x00: // end
         case 0x01:
         case 0x03:
         case 0x04:
         case 0x05:
         case 0x09: // undocumented?
         case 0x0B:
         case 0x0C:
         case 0x17:
         case 0x20:
            i = 4;
            break;
         case 0x02:
         case 0x0D:
         case 0x0E:
         case 0x14:
         case 0x15:
         case 0x16:
         case 0x18:
         case 0x19: // undocumented?
         case 0x1D:
            i = 8;
            break;
         case 0x08:
         case 0x11: // this is a guess, looking at 0x26A170
         case 0x13:
         case 0x1C:
            i = 12;
            break;
         case 0x10:
            i = 16;
            break;
         case 0x0F: // Kaze has 8
            i = 20;
            break;
         case 0x0A:
            i = 8;
            if (data[a+1]) {
               i += 4;
            }
            break;
         default:
            i = 4;
            fprintf(stderr, "WHY? %06X %2X\n", a, data[a]);
      }
      if (data[a] == 0x05 && indent > 1) {
         indent -= 2;
      }
      fprintf(out, ".byte ");
      print_spaces(out, indent);
      fprint_hex_source(out, &data[a], i);
      fprintf(out, "\n");
      // TODO: remove this debug
      if (sec->start == 0x269EA0) {
         printf("%06X: ", a);
         print_spaces(stdout, indent);
         printf("[ ");
         print_hex(&data[a], i);
         printf("]\n");
      }
      if (data[a] == 0x04) {
         indent += 2;
      }
      a += i;
   }
}

static void disassemble_section(FILE *out, unsigned char *data, long len, split_section *sec, proc_table *procs, rom_config *config)
{
   // disassemble all the procedures
   unsigned int ram_address;
   unsigned int last_end;
   unsigned int end_address;
   int start_proc;
   int proc_idx;
   // find first procedure in section
   last_end = rom_to_ram(config, sec->start);
   for (start_proc = 0; start_proc < procs->count; start_proc++) {
      if (procs->procedures[start_proc].start >= last_end) {
         break;
      }
   }
   // disassemble each procedure
   end_address = rom_to_ram(config, sec->end);
   for (proc_idx = start_proc; proc_idx < procs->count; proc_idx++) {
      ram_address = procs->procedures[proc_idx].start;
      if (ram_address > last_end) {
         // TODO: put larger sections in .bins
         fprintf(out, "\n# unknown assembly section %X-%X (%06X-%06X) [%X]",
               last_end, ram_address, ram_to_rom(config, last_end), ram_to_rom(config, ram_address), ram_address - last_end);
         unsigned int a = ram_to_rom(config, last_end);
         int count = 0;
         while (a < ram_to_rom(config, ram_address)) {
            if ((count % 4) == 0) {
               fprintf(out, "\n .word 0x%08x", read_u32_be(&data[a]));
            } else {
               fprintf(out, ", 0x%08x", read_u32_be(&data[a]));
            }
            a += 4;
            count++;
         }
         fprintf(out, "\n# end unknown section\n");
      } else if (ram_address < last_end) {
         fprintf(stderr, "Warning: %08X < %08X\n", ram_address, last_end);
      }
      // TODO: this is a workaround for the inner procedures __osPopThread, __osEnqueueThread, proc_80327D68
      if (procs->procedures[proc_idx].start != 0x80327D58 &&
          procs->procedures[proc_idx].start != 0x80327D68 &&
          procs->procedures[proc_idx].start != 0x80327D10) {
         disassemble_proc(out, data, len, &procs->procedures[proc_idx], config);
      }
      last_end = procs->procedures[proc_idx].end;
      if (last_end >= end_address) {
         break;
      }
   }
}

static void split_file(unsigned char *data, unsigned int length, proc_table *procs, rom_config *config)
{
#define GEN_DIR     "gen"
#define MAKEFILENAME GEN_DIR "/Makefile.gen"
#define BIN_DIR      GEN_DIR "/bin"
#define MIO0_DIR     GEN_DIR "/bin"
#define TEXTURE_DIR  GEN_DIR "/textures"
#define LEVEL_DIR    GEN_DIR "/levels"

   char asmfilename[512];
   char outfilename[512];
   char outfilepath[512];
   char mio0filename[512];
   char start_label[256];
   char end_label[256];
   char maketmp[256];
   char *makeheader_mio0;
   char *makeheader_level;
   FILE *fasm;
   FILE *fmake;
   int i, j, s;
   unsigned int w, h;
   unsigned int last_end = 0;
   unsigned int ptr_start, ptr_end;
   int count;
   int level_alloc;
   split_section *sections = config->sections;

   // create directories
   make_dir(GEN_DIR);
   make_dir(BIN_DIR);
   make_dir(MIO0_DIR);
   make_dir(TEXTURE_DIR);
   make_dir(LEVEL_DIR);

   // open main assembly file and write header
   sprintf(asmfilename, "%s/%s.s", GEN_DIR, config->basename);
   fasm = fopen(asmfilename, "w");
   if (fasm == NULL) {
      ERROR("Error opening %s\n", asmfilename);
      exit(3);
   }
   fprintf(fasm, asm_header);

   for (s = 0; s < config->section_count; s++) {
      split_section *sec = &sections[s];

      // error checking
      if (sec->start >= length || sec->end > length) {
         fprintf(stderr, "Error: section past end: 0x%X, 0x%X (%s) > 0x%X\n",
               sec->start, sec->end, sec->label ? sec->label : "", length);
         exit(4);
      }

      // fill gaps between regions
      // TODO: small gaps just .byte?
      if (sec->start != last_end) {
         sprintf(outfilename, "%s/%s.%06X.bin", BIN_DIR, config->basename, last_end);
         write_file(outfilename, &data[last_end], sec->start - last_end);
         fprintf(fasm, "L%06X:\n", last_end);
         fprintf(fasm, ".incbin \"%s\"\n", outfilename);
      }

      switch (sec->type)
      {
         case TYPE_HEADER:
            fprintf(fasm, ".section .header\n"
                          ".byte  0x%02X", data[sec->start]);
            for (i = 1; i < 4; i++) {
               fprintf(fasm, ", 0x%02X", data[sec->start + i]);
            }
            fprintf(fasm, " # PI BSD Domain 1 register\n");
            fprintf(fasm, ".word  0x%08X # clock rate setting\n", read_u32_be(&data[sec->start + 0x4]));
            fprintf(fasm, ".word  0x%08X # entry point\n", read_u32_be(&data[sec->start + 0x8]));
            fprintf(fasm, ".word  0x%08X # release\n", read_u32_be(&data[sec->start + 0xc]));
            fprintf(fasm, ".word  0x%08X # checksum1\n", read_u32_be(&data[sec->start + 0x10]));
            fprintf(fasm, ".word  0x%08X # checksum2\n", read_u32_be(&data[sec->start + 0x14]));
            fprintf(fasm, ".word  0x%08X # unknown\n", read_u32_be(&data[sec->start + 0x18]));
            fprintf(fasm, ".word  0x%08X # unknown\n", read_u32_be(&data[sec->start + 0x1C]));
            fprintf(fasm, ".ascii \"");
            fwrite(&data[sec->start + 0x20], 1, 20, fasm);
            fprintf(fasm, "\" # ROM name: 20 bytes\n");
            fprintf(fasm, ".word  0x%08X # unknown\n", read_u32_be(&data[sec->start + 0x34]));
            fprintf(fasm, ".word  0x%08X # cartridge\n", read_u32_be(&data[sec->start + 0x38]));
            fprintf(fasm, ".ascii \"");
            fwrite(&data[sec->start + 0x3C], 1, 2, fasm);
            fprintf(fasm, "\"       # cartridge ID\n");
            fprintf(fasm, ".ascii \"");
            fwrite(&data[sec->start + 0x3E], 1, 1, fasm);
            fprintf(fasm, "\"        # country\n");
            fprintf(fasm, ".byte  0x%02X       # version\n\n", data[sec->start + 0x3F]);
            fprintf(fasm, ".text\n\n");
            break;
         case TYPE_BIN:
            if (sec->label == NULL || sec->label[0] == '\0') {
               sprintf(outfilename, "%s/%s.%06X.bin", BIN_DIR, config->basename, sec->start);
            } else {
               sprintf(outfilename, "%s/%s.%06X.%s.bin", BIN_DIR, config->basename, sec->start, sec->label);
            }
            write_file(outfilename, &data[sec->start], sec->end - sec->start);
            if (sec->label == NULL || sec->label[0] == '\0' || strcmp(sec->label, "header") != 0) {
               if (sec->label == NULL || sec->label[0] == '\0') {
                  sprintf(start_label, "L%06X", sec->start);
               } else {
                  strcpy(start_label, sec->label);
               }
               fprintf(fasm, "%s:\n", start_label);
               fprintf(fasm, ".incbin \"%s\"\n", outfilename);
               fprintf(fasm, "%s_end:\n", start_label);
            }
            break;
         case TYPE_MIO0:
            // fill previous MIO0 blocks
            fprintf(fasm, ".space 0x%05x, 0x01 # %s\n", sec->end - sec->start, sec->label);
            break;
         case TYPE_PTR:
            ptr_start = read_u32_be(&data[sec->start]);
            ptr_end = read_u32_be(&data[sec->start + 4]);
            i = lookup_start(sections, config->section_count, ptr_start);
            if (i < 0) {
               sprintf(start_label, "L%06X", ptr_start);
            } else {
               strcpy(start_label, sections[i].label);
            }
            i = lookup_end(sections, config->section_count, ptr_end);
            if (i < 0) {
               sprintf(end_label, "L%06X", ptr_end);
            } else {
               sprintf(end_label, "%s_end", sections[i].label);
            }
            fprintf(fasm, ".word %s, %s\n", start_label, end_label);
            break;
         case TYPE_ASM:
            // TODO: this should be read from the ROM configuration
            switch (sec->start) {
               case 0x0F5580:
               case 0x22412C:
                  fprintf(fasm, "\n.section .text0x%08X, \"ax\"\n\n", rom_to_ram(config, sec->start));
                  break;
               default: break;
            }
            disassemble_section(fasm, data, length, sec, procs, config);
            break;
         case TYPE_LEVEL:
            // TODO: some level scripts can't be relocated yet
            if ((strcmp(sec->label, "game_over_level") == 0) ||
                (strcmp(sec->label, "main_menu_level") == 0) ||
                (strcmp(sec->label, "main_level_scripts") == 0)) {
               fprintf(fasm, "\n.global %s\n", sec->label);
               fprintf(fasm, "\n.global %s_end\n", sec->label);
               fprintf(fasm, "%s: # 0x%X\n", sec->label, sec->start);
               write_level(fasm, data, sections, config->section_count, s);
               fprintf(fasm, "%s_end:\n", sec->label);
            } else {
               fprintf(fasm, ".space 0x%05x, 0x01 # %s\n", sec->end - sec->start, sec->label);
            }
            break;
         case TYPE_BEHAVIOR:
            // behaviors are done below
            fprintf(fasm, ".space 0x%05x, 0x01 # %s\n", sec->end - sec->start, sec->label);
            break;
         default:
            ERROR("Don't know what to do with type %d\n", sec->type);
            exit(1);
            break;
      }
      last_end = sec->end;
   }

   // put MIO0 in separate data section and generate Makefile
   // allocate some space for the .bin makefile targets
   count = 1024; // header space
   level_alloc = 1024;
   i = strlen(" \\\n$(MIO0_DIR)/");
   j = strlen(" \\\n$(LEVEL_DIR)/");
   for (s = 0; s < config->section_count; s++) {
      split_section *sec = &sections[s];
      switch (sec->type) {
         case TYPE_MIO0:
            count += i + strlen(sec->label) + 4;
            break;
         case TYPE_LEVEL:
            level_alloc += j + strlen(sec->label) + 4;
            break;
         default:
            break;
      }
   }

   makeheader_mio0 = malloc(count);
   sprintf(makeheader_mio0, "MIO0_FILES =");

   makeheader_level = malloc(level_alloc);
   sprintf(makeheader_level, "LEVEL_FILES =");

   fmake = fopen(MAKEFILENAME, "w");
   fprintf(fmake, "MIO0_DIR = %s\n\n", MIO0_DIR);
   fprintf(fmake, "TEXTURE_DIR = %s\n\n", TEXTURE_DIR);
   fprintf(fmake, "LEVEL_DIR = %s\n\n", LEVEL_DIR);

   fprintf(fasm, "\n.section .mio0\n");
   for (s = 0; s < config->section_count; s++) {
      split_section *sec = &sections[s];
      switch (sec->type) {
         case TYPE_MIO0:
         {
            char binfilename[512];
            if (sec->label == NULL || sec->label[0] == '\0') {
               sprintf(outfilename, "%06X.mio0", sec->start);
            } else {
               sprintf(outfilename, "%s.mio0", sec->label);
            }
            sprintf(mio0filename, "%s/%s", MIO0_DIR, outfilename);
            write_file(mio0filename, &data[sec->start], sec->end - sec->start);
            if (sec->label == NULL || sec->label[0] == '\0') {
               sprintf(start_label, "L%06X", sec->start);
            } else {
               strcpy(start_label, sec->label);
            }
            fprintf(fasm, ".align 4, 0x01\n");
            fprintf(fasm, ".global %s\n", start_label);
            fprintf(fasm, "%s:\n", start_label);
            fprintf(fasm, ".incbin \"%s\"\n", mio0filename);
            fprintf(fasm, "%s_end:\n", start_label);
            // append to Makefile
            sprintf(maketmp, " \\\n$(MIO0_DIR)/%s", outfilename);
            strcat(makeheader_mio0, maketmp);
            if (sec->label == NULL || sec->label[0] == '\0') {
               sprintf(binfilename, "%s/%06X.bin", MIO0_DIR, sec->start);
            } else {
               sprintf(binfilename, "%s/%s.bin", MIO0_DIR, sec->label);
            }

            // extract MIO0 data
            mio0_decode_file(mio0filename, 0, binfilename);

            // extract texture data
            if (sec->extra) {
               texture *texts = sec->extra;
               int t;
               unsigned int offset = 0;
               sprintf(outfilepath, "%s/%s", TEXTURE_DIR, sec->label);
               make_dir(outfilepath);
               if (sec->label == NULL || sec->label[0] == '\0') {
                  fprintf(fmake, "$(MIO0_DIR)/%06X.bin:", sec->start);
               } else {
                  fprintf(fmake, "$(MIO0_DIR)/%s.bin: ", sec->label);
               }
               printf("Extracting textures from %s\n", sec->label);
               for (t = 0; t < sec->extra_len; t++) {
                  w = texts[t].width;
                  h = texts[t].height;
                  offset = texts[t].offset;
                  switch (texts[t].format) {
                     case FORMAT_IA:
                     {
                        ia *img = file2ia(binfilename, offset, w, h, texts[t].depth);
                        if (img) {
                           sprintf(outfilepath, "%s/%s/0x%05X.ia%d.png", TEXTURE_DIR, sec->label, offset, texts[t].depth);
                           ia2png(img, w, h, outfilepath);
                           free(img);
                           fprintf(fmake, " %s", outfilepath);
                        }
                        break;
                     }
                     case FORMAT_RGBA:
                     {
                        rgba *img = file2rgba(binfilename, offset, w, h);
                        if (img) {
                           sprintf(outfilepath, "%s/%s/0x%05X.png", TEXTURE_DIR, sec->label, offset);
                           rgba2png(img, w, h, outfilepath);
                           free(img);
                           fprintf(fmake, " %s", outfilepath);
                        }
                        break;
                     }
                     case FORMAT_SKYBOX:
                     {
                        // read in grid of MxN 32x32 tiles and save them as M*31xN*31 image
                        rgba *img;
                        unsigned int sky_offset = offset;
                        int m, n;
                        int tx, ty;
                        m = w/32;
                        n = h/32;
                        img = malloc(w*h*sizeof(rgba));
                        w -= m; // adjust for overlap
                        h -= n;
                        for (ty = 0; ty < n; ty++) {
                           for (tx = 0; tx < m; tx++) {
                              rgba *tile = file2rgba(binfilename, sky_offset, 32, 32);
                              int cx, cy;
                              for (cy = 0; cy < 31; cy++) {
                                 for (cx = 0; cx < 31; cx++) {
                                    int out_off = 31*w*ty + 31*tx + w*cy + cx;
                                    int in_off = 32*cy+cx;
                                    img[out_off] = tile[in_off];
                                 }
                              }
                              free(tile);
                              sky_offset += 32*32*2;
                           }
                        }
                        sprintf(outfilepath, "%s/%s/0x%05X.skybox.png", TEXTURE_DIR, sec->label, offset);
                        rgba2png(img, w, h, outfilepath);
                        free(img);
                        fprintf(fmake, " %s", outfilepath);
                        break;
                     }
                     default:
                        ERROR("Don't know what to do with type %d\n", sec->type);
                        exit(1);
                  }
               }
               fprintf(fmake, "\n\t$(N64GRAPHICS) $@ $^\n\n");
            }
#if GENERATE_ALL_PNG
            sprintf(outfilepath, "%s/%s", TEXTURE_DIR, sec->label);
            make_dir(outfilepath);
            w = 32;
            h = filesize(binfilename) / (w * 2);
            rgba *img = file2rgba(binfilename, 0, w, h);
            if (img) {
               sprintf(outfilepath, "%s/%s/ALL.png", TEXTURE_DIR, sec->label);
               rgba2png(img, w, h, outfilepath);
               free(img);
               img = NULL;
            }
#endif // GENERATE_ALL_PNG
            // touch bin, then mio0 files so 'make' doesn't rebuild them right away
            touch_file(binfilename);
            touch_file(mio0filename);
            break;
         }
         case TYPE_LEVEL:
            // TODO: some level scripts can't be relocated yet
            if ((strcmp(sec->label, "game_over_level") != 0) &&
                (strcmp(sec->label, "main_menu_level") != 0) &&
                (strcmp(sec->label, "main_level_scripts") != 0)) {
               FILE *flevel;
               char levelfilename[512];
               if (sec->label == NULL || sec->label[0] == '\0') {
                  sprintf(levelfilename, "%06X.s", sec->start);
               } else {
                  sprintf(levelfilename, "%s.s", sec->label);
               }
               sprintf(outfilename, "%s/%s", LEVEL_DIR, levelfilename);

               // decode and write level data out
               flevel = fopen(outfilename, "w");
               if (flevel == NULL) {
                  perror(outfilename);
                  exit(1);
               }

               write_level(flevel, data, sections, config->section_count, s);

               fclose(flevel);

               if (sec->label == NULL || sec->label[0] == '\0') {
                  sprintf(start_label, "L%06X", sec->start);
               } else {
                  strcpy(start_label, sec->label);
               }
               fprintf(fasm, ".align 4, 0x01\n");
               fprintf(fasm, ".global %s\n", start_label);
               fprintf(fasm, "%s:\n", start_label);
               fprintf(fasm, ".include \"%s\"\n", outfilename);
               fprintf(fasm, "%s_end:\n", start_label);
               // append to Makefile
               sprintf(maketmp, " \\\n$(LEVEL_DIR)/%s", levelfilename);
               strcat(makeheader_level, maketmp);
            }
            break;
         case TYPE_BEHAVIOR:
            fprintf(fasm, "\n.global %s\n", sec->label);
            fprintf(fasm, "\n.global %s_end\n", sec->label);
            fprintf(fasm, "%s: # 0x%X\n", sec->label, sec->start);
            write_behavior(fasm, data, sections, config->section_count, s);
            fprintf(fasm, "%s_end:\n", sec->label);
            break;
         default:
            break;
      }
   }
   fprintf(fmake, "\n\n%s", makeheader_mio0);
   fprintf(fmake, "\n\n%s", makeheader_level);

   // cleanup
   free(makeheader_mio0);
   free(makeheader_level);
   fclose(fmake);
   fclose(fasm);
}

static void print_usage(void)
{
   ERROR("n64split ROM CONFIG\n");
}

int main(int argc, char *argv[])
{
   rom_config config;
   proc_table procs;
   long len;
   unsigned char *data;
   int ret_val;

   memset(&procs, 0, sizeof(procs));
   procs.count = 0;

   if (argc < 3) {
      print_usage();
      return 1;
   }

   len = read_file(argv[1], &data);

   if (len <= 0) {
      return 2;
   }

   ret_val = parse_config_file(argv[2], &config);
   if (ret_val != 0) {
      return 2;
   }
   if (validate_config(&config, len)) {
      return 3;
   }

   // fill procs table from config labels
   mipsdisasm_add_procs(&procs, &config, len);
   // first pass disassembler
   mipsdisasm_pass1(data, len, &procs, &config);

   split_file(data, len, &procs, &config);

   return 0;
}

