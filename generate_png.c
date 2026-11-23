#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#define WIDTH 23171u
#define HEIGHT 23171u
#define ROW_BYTES (WIDTH * 2u + 1u)
#define OUTPUT_BYTES 65536u

static int write_bytes(FILE *file, const void *data, size_t length) {
    return fwrite(data, 1, length, file) == length;
}

static int write_u32be(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {
        (unsigned char)(value >> 24), (unsigned char)(value >> 16),
        (unsigned char)(value >> 8), (unsigned char)value
    };
    return write_bytes(file, bytes, sizeof bytes);
}

static int write_chunk(FILE *file, const char type[4],
                       const unsigned char *payload, uint32_t length) {
    uLong checksum = crc32(0L, Z_NULL, 0);
    checksum = crc32(checksum, (const Bytef *)type, 4);
    if (length != 0) checksum = crc32(checksum, payload, length);
    return write_u32be(file, length) && write_bytes(file, type, 4) &&
           write_bytes(file, payload, length) && write_u32be(file, (uint32_t)checksum);
}

static int write_compressed(FILE *file, z_stream *stream, int flush,
                            unsigned char output[OUTPUT_BYTES],
                            uint32_t *length, uLong *checksum) {
    int result;
    do {
        stream->next_out = output;
        stream->avail_out = OUTPUT_BYTES;
        result = deflate(stream, flush);
        if (result != Z_OK && result != Z_STREAM_END) return -1;

        size_t produced = OUTPUT_BYTES - stream->avail_out;
        if (produced > UINT32_MAX - *length || !write_bytes(file, output, produced)) return -1;
        *length += (uint32_t)produced;
        *checksum = crc32(*checksum, output, (uInt)produced);
    } while (stream->avail_in != 0 || stream->avail_out == 0 ||
             (flush == Z_FINISH && result != Z_STREAM_END));
    return result;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s output.png\n", argv[0]);
        return EXIT_FAILURE;
    }

    static const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char ihdr[13] = {
        (unsigned char)(WIDTH >> 24), (unsigned char)(WIDTH >> 16),
        (unsigned char)(WIDTH >> 8), (unsigned char)WIDTH,
        (unsigned char)(HEIGHT >> 24), (unsigned char)(HEIGHT >> 16),
        (unsigned char)(HEIGHT >> 8), (unsigned char)HEIGHT,
        16, 0, 0, 0, 0
    };
    unsigned char *row = calloc(ROW_BYTES, 1);
    unsigned char *output = malloc(OUTPUT_BYTES);
    if (!row || !output) {
        fprintf(stderr, "out of memory\n");
        free(row);
        free(output);
        return EXIT_FAILURE;
    }

    FILE *file = fopen(argv[1], "wb+");
    if (!file) {
        perror("fopen");
        free(row);
        free(output);
        return EXIT_FAILURE;
    }

    z_stream stream = {0};
    int zlib_ready = 0;
    int ok = 0;
    uint32_t compressed_length = 0;
    uLong checksum = crc32(0L, Z_NULL, 0);
    checksum = crc32(checksum, (const Bytef *)"IDAT", 4);

    if (!write_bytes(file, signature, sizeof signature) ||
        !write_chunk(file, "IHDR", ihdr, sizeof ihdr)) goto done;

    long idat_length_offset = ftell(file);
    if (idat_length_offset < 0 || !write_u32be(file, 0) ||
        !write_bytes(file, "IDAT", 4)) goto done;

    if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK) goto done;
    zlib_ready = 1;

    for (unsigned int y = 0; y < HEIGHT; ++y) {
        stream.next_in = row;
        stream.avail_in = ROW_BYTES;
        if (write_compressed(file, &stream, Z_NO_FLUSH, output,
                             &compressed_length, &checksum) < 0) goto done;
    }
    stream.next_in = Z_NULL;
    stream.avail_in = 0;
    if (write_compressed(file, &stream, Z_FINISH, output,
                         &compressed_length, &checksum) != Z_STREAM_END) goto done;

    if (!write_u32be(file, (uint32_t)checksum) ||
        !write_chunk(file, "IEND", NULL, 0) ||
        fseek(file, idat_length_offset, SEEK_SET) != 0 ||
        !write_u32be(file, compressed_length) || fflush(file) != 0) goto done;

    ok = 1;
    printf("Wrote %s: %u x %u, 16-bit grayscale, %u compressed bytes\n",
           argv[1], WIDTH, HEIGHT, compressed_length);

done:
    if (!ok) fprintf(stderr, "PNG generation failed\n");
    if (zlib_ready) deflateEnd(&stream);
    if (fclose(file) != 0) ok = 0;
    free(row);
    free(output);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
