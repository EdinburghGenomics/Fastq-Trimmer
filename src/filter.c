//
//  filter.c
//  Fastq-Filterer
//
//  Created by Murray Wham on 05/09/2016.
//  Copyright (c) 2017 Edinburgh Genomics (see ../LICENCE)
//

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <zlib.h>
#include <getopt.h>
#include <time.h>
#include <stdbool.h>
#include "filter.h"

#define block_size 2048
#define unsafe_block_size 4096

int threshold = -1;
bool quiet = false;
char *r1i_path = NULL, *r1o_path = NULL;
char *r2i_path = NULL, *r2o_path = NULL;
int read_pairs_checked = 0, read_pairs_removed = 0, read_pairs_remaining = 0;
int trim_r1, trim_r2;
char* remove_tiles;
char** tiles_to_remove;


static void _log(char* fmt_str, ...) {
    if (quiet) {
        return;
    }
    
    time_t t = time(NULL);
    struct tm* now = localtime(&t);
    
    printf(
        "[%i-%i-%i %i:%i:%i][fastq_filterer] ",
        now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min,  now->tm_sec
    );
    va_list args;
    va_start(args, fmt_str);
    vprintf(fmt_str, args);
    va_end(args);
    
}


static char* readln_unsafe(gzFile* f) {
    char* line = malloc(unsafe_block_size);
    line[0] = '\0';
    gzgets(f, line, unsafe_block_size);
    return line;
}


static char* readln(gzFile* f) {
    /*
     Read a line from a file. Each time this function is called on a file, the next
     line is read. Memory is dynamically allocated to allow reading of lines of any
     length.
     
     :input FILE* f: The file to read from
     :output: char* line (the line read), or null '\0' string if no input is available.
     */
    
    int _block_size = block_size;
    char* line = malloc(sizeof (char) * _block_size);
    line[0] = '\0';
    
    do {
        _block_size += block_size;
        line = realloc(line, _block_size + 1);
        char* _line_part = malloc(sizeof (char) * block_size);
        _line_part[0] = '\0';

        if (gzgets(f, _line_part, block_size)) {
            strcat(line, _line_part);
            free(_line_part);
        } else {
            free(_line_part);
            return line;
        }
    } while (line[strlen(line) - 1] != '\n');

    return line;
}


char* (*read_func)(gzFile*) = readln;


static char* get_tile_id(char* fastq_header) {
    char* field;
    field = strtok(fastq_header, ":");
    int i;
    for (i=0; i<4; i++) {
        field = strtok(NULL, ":");  // walk along the header 4 times to the tile ID
    }
    return field;
}



static bool std_check_read(char* r1_header, char* r1_seq, char* r1_strand, char* r1_qual, char* r2_header, char* r2_seq, char* r2_strand, char* r2_qual) {
    if ((strlen(r1_seq) > threshold) && (strlen(r2_seq) > threshold)) {
        return true;
    } else {
        return false;
    }
}


static bool tile_check_read(char* r1_header, char* r1_seq, char* r1_strand, char* r1_qual, char* r2_header, char* r2_seq, char* r2_strand, char* r2_qual) {
    int result = std_check_read(r1_header, r1_seq, r1_strand, r1_qual, r2_header, r2_seq, r2_strand, r2_qual);
    if (result == false) {
        return false;
    }
    
    // get_tile_id will modify the string passed to it with strtok, so use a copy
    char* _fq_header = malloc(sizeof (char) * (strlen(r1_header) + 1));
    strcpy(_fq_header, r1_header);
    char* tile_id = get_tile_id(_fq_header);
    int i = 0;
    
    char* comp = tiles_to_remove[i];
    
    bool ret_val = true;
    while (comp != NULL) {  // check for null terminator at end of remove_tiles
        if (strcmp(comp, tile_id) == 0) {
            ret_val = false;
        }
        i++;
        comp = tiles_to_remove[i];
    }
    
    free(_fq_header);
    return ret_val;
}


bool (*check_func)(char*, char*, char*, char*, char*, char*, char*, char*) = std_check_read;


static void std_include(char* header, char* seq, char* strand, char* qual, FILE* outfile) {
    fputs(header, outfile);
    fputs(seq, outfile);
    fputs(strand, outfile);
    fputs(qual, outfile);
}


static void _trim_include(char* header, char* seq, char* strand, char* qual, FILE* outfile, int trim_len) {
    if (strlen(seq) > trim_len + 1) {  // add 1 here to compensate for \n at end of line...
        seq[trim_len] = '\n';  // ...but 0-indexing means we don't need to add 1 here
        seq[trim_len + 1] = '\0';
        qual[trim_len] = '\n';
        qual[trim_len + 1] = '\0';
    }
    std_include(header, seq, strand, qual, outfile);
}

static void trim_include_r1(char* header, char* seq, char* strand, char* qual, FILE* outfile) {
    _trim_include(header, seq, strand, qual, outfile, trim_r1);
}


static void trim_include_r2(char* header, char* seq, char* strand, char* qual, FILE* outfile) {
    _trim_include(header, seq, strand, qual, outfile, trim_r2);
}


void (*include_func_r1)(char*, char*, char*, char*, FILE*) = std_include;
void (*include_func_r2)(char*, char*, char*, char*, FILE*) = std_include;


static int filter_fastqs() {
    /*
     Read two fastqs (R1.fastq, R2.fastq) entry by entry, check whether the R1 and R2 for each read
     are both long enough, and output them to R1_filtered.fastq and R2_filtered.fastq if they are.
     
     :input char* r1_path: Path to R1.fastq input file
     :input char* r1_filtered: Path to R1_filtered.fastq output file
     :input char* r2_path: Path to R1.fastq input file
     :input char* r2_filtered: Path to R1_filtered.fastq output file
     */
    
    gzFile* r1i = gzopen(r1i_path, "r");
    gzFile* r2i = gzopen(r2i_path, "r");
    FILE* r1o = fopen(r1o_path, "w");
    FILE* r2o = fopen(r2o_path, "w");
    
    char* r1_header;
    char* r1_seq;
    char* r1_strand;
    char* r1_qual;

    char* r2_header;
    char* r2_seq;
    char* r2_strand;
    char* r2_qual;
    
    while (true) {
        r1_header = read_func(r1i);  // @read_1 1
        r1_seq = read_func(r1i);     // ATGCATGC
        r1_strand = read_func(r1i);  // +
        r1_qual = read_func(r1i);    // #--------

        r2_header = read_func(r2i);  // @read_1 2
        r2_seq = read_func(r2i);     // ATGCATGC
        r2_strand = read_func(r2i);  // -
        r2_qual = read_func(r2i);    // #--------
        
        if (*r1_header == '\0' || *r2_header == '\0') {
            int ret_val = 0;
            if (*r1_header != *r2_header) {  // if either file is not finished
                _log("Input fastqs have differing numbers of reads, from line %i\n", read_pairs_checked * 4);
                ret_val = 1;
            }
            
            free(r1_header);
            free(r1_seq);
            free(r1_strand);
            free(r1_qual);
            
            free(r2_header);
            free(r2_seq);
            free(r2_strand);
            free(r2_qual);
            
            gzclose(r1i);
            gzclose(r2i);
            fclose(r1o);
            fclose(r2o);

            return ret_val;

        } else if (check_func(r1_header, r1_seq, r1_strand, r1_qual, r2_header, r2_seq, r2_strand, r2_qual) == true) {
            // include reads
            read_pairs_checked++;
            read_pairs_remaining++;
            
            include_func_r1(r1_header, r1_seq, r1_strand, r1_qual, r1o);
            include_func_r2(r2_header, r2_seq, r2_strand, r2_qual, r2o);
        } else {
            // exclude reads
            read_pairs_checked++;
            read_pairs_removed++;
        }
        
        free(r1_header);
        free(r1_seq);
        free(r1_strand);
        free(r1_qual);
        
        free(r2_header);
        free(r2_seq);
        free(r2_strand);
        free(r2_qual);
    }
}


static char* build_output_path(char* input_path) {
    /*
     Convert, e.g, basename.fastq to basename_filtered.fastq. Used when output fastq paths are not specified.
     */
    
    int file_ext_len = 6;  // .fastq
    
    if (input_path[strlen(input_path) - 1] == 'z') {
        file_ext_len = 9;  // .fastq.gz
    }
    
    size_t basename_len = strlen(input_path) - file_ext_len;
    char* output_path = malloc(sizeof (char) * (basename_len + 15));  // _filtered.fastq
    strncpy(output_path, input_path, basename_len);
    output_path[basename_len] = '\0';
    strcat(output_path, "_filtered.fastq");
    return output_path;
}


static void build_remove_tiles() {
    if (remove_tiles == NULL) {  // no --remove_tiles argument
        return;
    }
    
    char* rm_tiles = malloc(sizeof (char) * (strlen(remove_tiles) + 1));
    strcpy(rm_tiles, remove_tiles);  // use a copy for strtok
    
    int ntiles = 1;
    char* comma = strchr(rm_tiles, ',');
    while (comma != NULL) {
        ntiles++;
        comma = strchr(comma+1, ',');
    }
    
    tiles_to_remove = malloc(sizeof (char*) * (ntiles + 1));
    int i = 0;
    char* field;
    field = strtok(rm_tiles, ",");
    while (field != NULL) {
        tiles_to_remove[i] = malloc(sizeof (char) * strlen(field) + 1);
        strcpy(tiles_to_remove[i], field);
        field = strtok(NULL, ",");
        i++;
    }
    tiles_to_remove[i] = NULL;  // set a null terminator
    free(rm_tiles);
}


static void output_stats(char* stats_file) {
    FILE* f = fopen(stats_file, "w");
    
    fprintf(
        f,
        "r1i %s\nr1o %s\nr2i %s\nr2o %s\n",
        r1i_path, r1o_path, r2i_path, r2o_path
    );
    fprintf(
        f,
        "read_pairs_checked %i\nread_pairs_removed %i\nread_pairs_remaining %i\n",
        read_pairs_checked, read_pairs_removed, read_pairs_remaining
    );
    
    if (trim_r1) {
        fprintf(f, "trim_r1 %i\n", trim_r1);
    }
    if (trim_r2) {
        fprintf(f, "trim_r2 %i\n", trim_r2);
    }
    if (remove_tiles) {
        fprintf(f, "remove_tiles %s\n", remove_tiles);
    }
    
    fclose(f);
}



int main(int argc, char* argv[]) {
    int arg;
    
    static struct option args[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"unsafe", no_argument, 0, 'f'},
        {"stats_file", required_argument, 0, 's'},
        {"threshold", required_argument, 0, 't'},
        {"remove_tiles", required_argument, 0, 'r'},
        {"trim_r1", required_argument, 0, 'l'},
        {"trim_r2", required_argument, 0, 'm'},
        {"i1", required_argument, 0, 'i'},
        {"i2", required_argument, 0, 'j'},
        {"o1", required_argument, 0, 'o'},
        {"o2", required_argument, 0, 'p'},
        {0, 0, 0, 0}
    };
    int opt_idx = 0;
    char* stats_file = NULL;
    
    while ((arg = getopt_long(argc, argv, "", args, &opt_idx)) != -1) {
        switch(arg) {
            case 'h':
                printf(USAGE);
                exit(0);
                break;
            case 'v':
                printf("%s\n", VERSION);
                exit(0);
                break;
            case 'q':
                quiet = true;
                break;
            case 'f':
                read_func = readln_unsafe;
                break;
            case 's':
                stats_file = malloc(sizeof (char) * (strlen(optarg) + 1));
                strcpy(stats_file, optarg);
                break;
            case 't':
                threshold = atoi(optarg);
                break;
            case 'r':
                remove_tiles = malloc(sizeof (char) * (strlen(optarg) + 1));
                strcpy(remove_tiles, optarg);
                build_remove_tiles();
                check_func = tile_check_read;
                break;
            case 'l':
                trim_r1 = atoi(optarg);
                include_func_r1 = trim_include_r1;
                break;
            case 'm':
                trim_r2 = atoi(optarg);
                include_func_r2 = trim_include_r2;
                break;
            case 'i':
                r1i_path = malloc(sizeof (char) * (strlen(optarg) + 1));
                strcpy(r1i_path, optarg);
                break;
            case 'j':
                r2i_path = malloc(sizeof (char) * (strlen(optarg) + 1));
                strcpy(r2i_path, optarg);
                break;
            case 'o':
                r1o_path = malloc(sizeof (char) * (strlen(optarg) + 1));
                strcpy(r1o_path, optarg);
                break;
            case 'p':
                r2o_path = malloc(sizeof (char) * (strlen(optarg) + 1));
                strcpy(r2o_path, optarg);
                break;
            default:
                exit(1);
        }
    }
    
    if (r1i_path == NULL || r2i_path == NULL || threshold == -1) {
        printf("Missing required arguments: r1i, r2i, threshold\n");
        exit(1);
    }
    
    if (r1o_path == NULL) {
        _log("No o1 argument given - deriving from i1\n");
        r1o_path = build_output_path(r1i_path);
    }
    if (r2o_path == NULL) {
        _log("No o2 argument given - deriving from i2\n");
        r2o_path = build_output_path(r2i_path);
    }

    _log("R1: %s -> %s\n", r1i_path, r1o_path);
    _log("R2: %s -> %s\n", r2i_path, r2o_path);
    _log("Filter threshold: %i\n", threshold);
    if (trim_r1) {_log("Trimming R1 to %i\n", trim_r1);}
    if (trim_r2) {_log("Trimming R2 to %i\n", trim_r2);}
    if (remove_tiles) {_log("Removing tiles: %s\n", remove_tiles);}
    
    int exit_status = filter_fastqs();
    
    _log("Checked %i read pairs, %i removed, %i remaining. Exit status %i\n",
         read_pairs_checked, read_pairs_removed, read_pairs_remaining, exit_status);

    if (stats_file != NULL) {
        _log("Writing stats file %s\n", stats_file);
        output_stats(stats_file);
    }
    
    return exit_status;
}
