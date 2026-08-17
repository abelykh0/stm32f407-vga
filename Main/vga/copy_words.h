#ifndef COPY_WORDS_H
#define COPY_WORDS_H

#include <cstdint>

/*
 * Moves some number of aligned words using the fastest method I could think up.
 */
void copy_words(std::uint32_t const *source,
                std::uint32_t *dest,
                std::uint32_t count);

#endif  // COPY_WORDS_H
