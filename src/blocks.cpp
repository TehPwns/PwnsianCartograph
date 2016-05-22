#include "lodepng.h"
#include "json11.hpp"
#include "ZipLib/ZipFile.h"
#include "utility.h"
#include "blocks.h"

namespace blocks
{

BlockID::BlockID(int id, int meta)
    : id(id), meta(meta)
{

}

BlockID::BlockID(const std::string& string)
{
    //Example: "405-3.png" or "405-3"
    size_t dashpos = string.find('-'),
           dotpos  = string.find('.');

    /* There's no checks for == npos here, because by default
     * if they're not found, they'll be npos anyway. In that case,
     * we have something like "404" which is the ID, and meta
     * can default into 0 */
    id = std::atoi(string.substr(0,dashpos).c_str());

    if(dashpos != std::string::npos) {
        meta = std::atoi(string.substr(dashpos+1, dotpos).c_str());
    }
}

bool BlockID::operator<(const BlockID& other) const
{
   if(id < other.id)
       return true;
   else if(id == other.id)
       return (meta < other.meta);
   return false;
}

BlockID::operator std::string() const
{
    std::stringstream ss;
    ss << id << '-' << meta;
    return ss.str();
}

}

namespace blocks
{

BlockColors::BlockColors()
{
    rgba = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
}

BlockColors::~BlockColors()
{
    SDL_FreeFormat(rgba);
}

void BlockColors::load(const std::string& zipFileName, const std::string& cacheFileName)
{
    using namespace json11;

    //Keep filenames for later
    this->zipFileName = zipFileName;
    this->cacheFileName = cacheFileName;

    //Zip file interface
    auto archive = ZipFile::Open(zipFileName);
    if(archive == nullptr) {
        error("Could not find items archive", zipFileName);
    }

    //Cache JSON. This is a single { } of key:value of "blockid-meta" to .zip CRC and RGBA color
    // ex: {"2-4": {"crc": 5234231, "color": 2489974272}, ... }
    std::string parseErr;
    Json cacheJson = Json::parse(readFile(cacheFileName), parseErr);

    /* Will be true if computeColor is called, basically if cache is missing
     * or CRC changed in zip for any file */
    bool hadToRecompute = false;

    for(unsigned i = 0; i != archive->GetEntriesCount(); ++i)
    {
        /* Name and CRC of the block image in .zip
         * substr on the name is used to cut off the .png at the end */
        auto entry = archive->GetEntry(i);
        unsigned zipcrc = entry->GetCrc32();
        std::string name = entry->GetName();
        name = name.substr(0, name.find('.'));

        /* To get a block color, first the cache (cacheFileName, a .json file) is checked.
         * if it's not there, it's recomputed used computecolor. If that happens,
         * "hadToRecompute" is set to true, and a new .json cache will be written out */
        SDL_Color color;
        Json key = cacheJson[name];
        if(key.is_object() && key["crc"].number_value() == zipcrc)
        {
            Uint32 pixel = key["color"].number_value();
            SDL_GetRGBA(pixel, rgba, &color.r, &color.g, &color.b, &color.a);
        }
        else {
            color = computeColor(entry);
            hadToRecompute = true;
        }

        //Store color and CRC in this object
        blockColors[name] = {color, zipcrc};
    }

    //If any blocks were not found in cache
    if(hadToRecompute) {
        saveNewJsonCache();
    }
}

bool BlockColors::isLoaded() const
{
    return isLoaded();
}

SDL_Color BlockColors::computeColor(const ZipArchiveEntry::Ptr& blockImage)
{
    //The returned color
    SDL_Color ret;

    //The average pixel values during computation
    unsigned avgR = 0, avgG = 0, avgB = 0;

    //Get the raw PNG data from the .zip, and decode it into pixels
    std::vector<char> pngBytes = readZipEntry(blockImage);
    std::vector<unsigned char> pixels;
    unsigned w = 0, h = 0;
    lodepng::decode(pixels, w, h, (const unsigned char*)pngBytes.data(), pngBytes.size());
    int nPixels = w * h;

    //Add colors over the pixels that are not transparent
    for(unsigned i = 0; i != pixels.size(); i += 4) {
        Uint8 r = pixels[i+0],
         g = pixels[i+1],
         b = pixels[i+2],
         a = pixels[i+3];

        if(a != SDL_ALPHA_TRANSPARENT) {
            avgR += r;
            avgG += g;
            avgB += b;
        }
    }

    //Store average color, full alpha
    ret = {Uint8(avgR/nPixels), Uint8(avgG/nPixels), Uint8(avgB/nPixels), SDL_ALPHA_OPAQUE};

    return ret;
}

std::vector<char> BlockColors::readZipEntry(const ZipArchiveEntry::Ptr& blockImage)
{
    //This is different from readStream, size needs to be gotten this way
    size_t size = blockImage->GetSize();
    std::vector<char> content(size);

    //Read "size" bytes into string
    std::istream* stream = blockImage->GetDecompressionStream();
    stream->read(content.data(), size);

    return content;
}

void BlockColors::saveNewJsonCache() const
{
    std::ofstream file(cacheFileName);
    if(file.is_open())
    {
        file << "{\n";
        for(auto it = blockColors.begin(); it != blockColors.end(); ++it)
        {
            std::string id = it->first; //BlockID -> string conversion
            auto&& value = it->second;

            //Convience
            SDL_Color color = value.first;
            unsigned crc = value.second;
            Uint32 pixel = SDL_MapRGBA(rgba, color.r, color.g, color.b, color.a);

            //This is not fun.
            file << "\t\"" << id << "\":" << "{\"crc\":" << std::to_string(crc)
                 << ", \"color\":" << std::to_string(pixel) << "}";

            //Add a comma if not last element
            auto it2 = it;
            if(++it2 != blockColors.end()) {
                file << ',';
            }

            file << '\n';
        }
        file << "}";
    }
    file.close();
}

SDL_Color BlockColors::getBlockColor(unsigned id, unsigned meta) const
{
    auto it = blockColors.find(BlockID(id,meta));
    if(it != blockColors.end()) {
        return it->second.first;
    }
    return {0, 0, 0, 0}; //Block not known--return transparent color
}

//This forumla can be found on Wikipedia or similar, searching for "rgb to hsv"
SDL_Color BlockColors::rgb2hsv(const SDL_Color& rgb)
{
    int r = rgb.r, g = rgb.g, b = rgb.b;

    double maxC = b;
    if (maxC < g) maxC = g;
    if (maxC < r) maxC = r;
    double minC = b;
    if (minC > g) minC = g;
    if (minC > r) minC = r;

    double delta = maxC - minC;

    double V = maxC;
    double S = 0;
    double H = 0;

    if (delta == 0) {
        H = 0;
        S = 0;
    } else {
        S = delta / maxC;
        double dR = 60*(maxC - r)/delta + 180;
        double dG = 60*(maxC - g)/delta + 180;
        double dB = 60*(maxC - b)/delta + 180;
        if (r == maxC)
            H = dB - dG;
        else if (g == maxC)
            H = 120 + dR - dB;
        else
            H = 240 + dG - dR;
    }

    if (H<0)   H+=360;
    if (H>=360)H-=360;

    return {Uint8(H), Uint8(S), Uint8(V), SDL_ALPHA_OPAQUE};
}

}
