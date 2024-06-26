#include "ScrambledZipfGenerator.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
namespace utils
{
// -------------------------------------------------------------------------------------
u64 ScrambledZipfGenerator::rand(u64 zipf_value) {
   return min + (FNV::hash_u64(zipf_value) % n);
}
u64 ScrambledZipfGenerator::rand()
{
   u64 zipf_value = zipf_generator.rand();
   return min + (FNV::hash_u64(zipf_value) % n);
}
// -------------------------------------------------------------------------------------
u64 ScrambledZipfDistGenerator::rand(u64 zipf_value) {
   return min + (FNV::hash_u64(zipf_value) % n);
}
u64 ScrambledZipfDistGenerator::rand()
{
   u64 zipf_value = zipf_generator.rand();
   return rand(zipf_value);
}
// -------------------------------------------------------------------------------------
}  // namespace utils
}  // namespace leanstore
