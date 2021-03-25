/*=========================================================================

  Program:   Visualization Toolkit
  Module:    OMFFile.cxx
  Language:  C++

  Copyright (c) 1993-2002 Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "OMFFile.h"
#include "OMFHelpers.h"

#include "vtkArrayDispatch.h"
#include "vtkDataArray.h"
#include "vtkDoubleArray.h"
#include "vtkImageData.h"
#include "vtkPNGReader.h"
#include "vtkTypeInt64Array.h"

#include "vtk_jsoncpp.h"
#include "vtk_zlib.h"
#include "vtksys/FStream.hxx"
#include "vtksys/SystemTools.hxx"

namespace omf
{

namespace detail
{

// OMF doesn't appear to store any information about size of decompressed data
// so the approach here is to decompress into the correct vtk array type and
// resize the array as necessary to get everything.
struct DecompressToDataArrayWorker
{
  template <typename ArrayT>
  void operator()(ArrayT* array, z_stream* zStream, vtkIdType numComponents)
  {
    if (!zStream->next_in || zStream->avail_in == 0)
    {
      vtkGenericWarningMacro(<< "either next_in or avail_in was not set for zStream");
      return;
    }

    using ValueType = typename ArrayT::ValueType;

    // TODO is this a reasonable starting size for numTuples?
    vtkIdType numTuplesStart = 4096;
    vtkIdType numTuples = numTuplesStart;
    vtkIdType numValues = numTuples * numComponents;
    vtkIdType bufSizeBytes = numValues * sizeof(ValueType);
    vtkIdType destPos = 0;
    vtkIdType totalTuplesRead = 0;
    array->SetNumberOfComponents(numComponents);

    do
    {
      // preserve data already added
      array->Resize(numTuples);
      zStream->next_out = reinterpret_cast<unsigned char*>(array->WritePointer(destPos, numValues));
      zStream->avail_out = bufSizeBytes;
      auto err = inflate(zStream, Z_NO_FLUSH);
      if (err != Z_OK && err != Z_STREAM_END)
      {
        vtkGenericWarningMacro(<< "error (" << err << ") decompressing data");
        return;
      }
      vtkIdType numValuesRead = (bufSizeBytes - zStream->avail_out) / sizeof(ValueType);
      destPos += numValuesRead;
      totalTuplesRead += numValuesRead / numComponents;
      numTuples += numTuplesStart;
    } while (zStream->avail_in > 0);
    inflateEnd(zStream);

    // Perform resize because otherwise we may have some junk values at the end
    array->Resize(totalTuplesRead);
  }
};

void omfInflateInit(z_stream* zStream)
{
  zStream->zalloc = Z_NULL;
  zStream->zfree = Z_NULL;
  zStream->opaque = Z_NULL;

  if (inflateInit(zStream) != Z_OK)
  {
    vtkGenericWarningMacro(<< "inflateInit failed");
  }
}

char toHex(int x)
{
  return x < 10 ? x + '0' : (x - 10) + 'A';
}

std::string convertToUIDString(const char* buffer)
{
  std::string uid;
  for (int i = 0; i < 16; ++i)
  {
    if (i == 4 || i == 6 || i == 8 || i == 10)
    {
      uid += '-';
    }
    uid += toHex((buffer[i] >> 4) & 0x0F);
    uid += toHex(buffer[i] & 0x0F);
  }
  return vtksys::SystemTools::LowerCase(uid);
}

} // end namespace detail

struct OMFFile::FileImpl
{
  vtksys::ifstream* Stream;
  z_stream ZStream;
  unsigned long long FileLength;
  unsigned long long JSONStart;
  Json::Value JSONRoot;
};

OMFFile::OMFFile()
  : Impl(new FileImpl)
{
}

OMFFile::~OMFFile()
{
  if (Impl && this->Impl->Stream)
  {
    delete this->Impl->Stream;
  }
}

bool OMFFile::OpenStream(const char* filename)
{
  this->Impl->Stream = new vtksys::ifstream(filename, std::ios::binary);
  if (!this->Impl->Stream)
  {
    return false;
  }
  return true;
}

// read 60 byte OMF header that consists of:
// * magic number   - 4 bvtes
// * version string - 32 bytes
// * project uid    - 16 bytes (little endian)
// * JSON start     - 8 bytes (unsigned long long, little endian)
bool OMFFile::ReadHeader(std::string& uid)
{
  constexpr int HEADER_UID = 36;
  constexpr int HEADER_JSON = 52;
  constexpr int UID_LEN = 16;
  constexpr int MAGIC_NUM_LEN = 4;
  constexpr unsigned char MAGIC_NUM[MAGIC_NUM_LEN] = { 0x84, 0x83, 0x82, 0x81 };

  this->Impl->Stream->seekg(0, std::ios::end);
  this->Impl->FileLength = this->Impl->Stream->tellg();
  if (this->Impl->FileLength < 60)
  {
    vtkGenericWarningMacro(<< "OMF file is smaller than 60 bytes and invalid");
    return false;
  }

  unsigned char magicNum[MAGIC_NUM_LEN];
  this->Impl->Stream->seekg(0, std::ios::beg);
  this->Impl->Stream->read(reinterpret_cast<char*>(magicNum), MAGIC_NUM_LEN);
  for (int i = 0; i < MAGIC_NUM_LEN; ++i)
  {
    if (MAGIC_NUM[i] != magicNum[i])
    {
      vtkGenericWarningMacro(<< "Magic number in OMF header is incorrect");
      return false;
    }
  }

  this->Impl->Stream->seekg(HEADER_UID, std::ios::beg);
  char uidBuf[UID_LEN];
  this->Impl->Stream->read(uidBuf, UID_LEN);
  uid = detail::convertToUIDString(uidBuf);

  this->Impl->Stream->seekg(HEADER_JSON, std::ios::beg);
  this->Impl->Stream->read(
    reinterpret_cast<char*>(&this->Impl->JSONStart), sizeof(this->Impl->JSONStart));
  if (this->Impl->JSONStart >= this->Impl->FileLength)
  {
    vtkGenericWarningMacro(<< "JSON start is past the end of file");
    return false;
  }
  return true;
}

bool OMFFile::ParseJSON()
{
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;

  std::string formattedErrors;

  this->Impl->Stream->seekg(this->Impl->JSONStart, std::ios::beg);
  // parse the JSON portion of the file into the Json::Value root
  auto parsedSuccess =
    Json::parseFromStream(builder, *this->Impl->Stream, &this->Impl->JSONRoot, &formattedErrors);
  if (!parsedSuccess)
  {
    vtkGenericWarningMacro(<< "Failed to parse JSON" << endl << formattedErrors);
  }
  return parsedSuccess;
}

const Json::Value& OMFFile::JSONRoot()
{
  return this->Impl->JSONRoot;
}

vtkSmartPointer<vtkDataArray> OMFFile::ReadArrayFromStream(
  const std::string& uid, int numComponents /*= -1*/)
{
  const auto& json = this->Impl->JSONRoot[uid];
  if (json.isNull() || !json.isObject() || !json.isMember("array") || !json.isMember("__class__"))
  {
    return nullptr;
  }

  const auto& arrayJSON = json["array"];
  std::string dtype;
  helper::GetStringValue(arrayJSON["dtype"], dtype);
  unsigned int length, pos;
  helper::GetUIntValue(arrayJSON["length"], length);
  helper::GetUIntValue(arrayJSON["start"], pos);

  if (pos >= this->Impl->FileLength)
  {
    vtkGenericWarningMacro(<< "read position is past end of file");
    return nullptr;
  }

  if (numComponents < 1)
  {
    std::string arrayClass;
    helper::GetStringValue(json["__class__"], arrayClass);
    if (arrayClass == "ScalarArray")
    {
      numComponents = 1;
    }
    else if (arrayClass == "Vector2Array" || arrayClass == "Int2Array")
    {
      numComponents = 2;
    }
    else if (arrayClass == "Vector3Array" || arrayClass == "Int3Array")
    {
      numComponents = 3;
    }
    else
    {
      vtkGenericWarningMacro("Array class type " << arrayClass << " not supported");
      return nullptr;
    }
  }

  this->Impl->Stream->seekg(pos, std::ios::beg);
  char* compressedData = new char[length];
  this->Impl->Stream->read(compressedData, length);

  detail::omfInflateInit(&this->Impl->ZStream);

  this->Impl->ZStream.next_in = reinterpret_cast<unsigned char*>(compressedData);
  this->Impl->ZStream.avail_in = length;

  vtkSmartPointer<vtkDataArray> array = nullptr;
  // Looking through OMF code base, it seems these are the only possible data types
  if (dtype == "<f8")
  {
    array = vtkSmartPointer<vtkDoubleArray>::New();
  }
  else if (dtype == "<i8")
  {
    array = vtkSmartPointer<vtkTypeInt64Array>::New();
  }
  else if (dtype == "image/png")
  {
    vtkGenericWarningMacro(<< "type image/png should be read with OMFFile::ReadPNGFromStream()");
    return nullptr;
  }
  else
  {
    vtkGenericWarningMacro(<< "OMF data type '" << dtype << "' is not supported");
    return nullptr;
  }

  detail::DecompressToDataArrayWorker worker;
  using Dispatcher = vtkArrayDispatch::DispatchByValueType<vtkArrayDispatch::AllTypes>;

  if (!Dispatcher::Execute(array, worker, &this->Impl->ZStream, numComponents))
  {
    vtkGenericWarningMacro(<< "ArrayDispatch failed");
  }
  delete[] compressedData;
  return array;
}

vtkSmartPointer<vtkImageData> OMFFile::ReadPNGFromStream(const Json::Value& json)
{
  std::string dtype;
  helper::GetStringValue(json["dtype"], dtype);
  unsigned int length, pos;
  helper::GetUIntValue(json["length"], length);
  helper::GetUIntValue(json["start"], pos);

  if (dtype != "image/png")
  {
    return nullptr;
  }
  if (pos >= this->Impl->FileLength)
  {
    vtkGenericWarningMacro(<< "read position is past end of file");
    return nullptr;
  }

  this->Impl->Stream->seekg(pos, std::ios::beg);
  char* compressedData = new char[length];
  this->Impl->Stream->read(compressedData, length);

  detail::omfInflateInit(&this->Impl->ZStream);

  this->Impl->ZStream.next_in = reinterpret_cast<unsigned char*>(compressedData);
  this->Impl->ZStream.avail_in = length;

  // TODO check that we've read the whole thing
  // This function only used for textures, so will make improvements
  // when finishing texture functionality in future MR
  unsigned int outputSize = length * 4;
  char* decompressed = new char[outputSize];
  this->Impl->ZStream.next_out = reinterpret_cast<unsigned char*>(decompressed);
  this->Impl->ZStream.avail_out = outputSize;

  auto err = inflate(&this->Impl->ZStream, Z_NO_FLUSH);
  if (err != Z_OK && err != Z_STREAM_END)
  {
    vtkGenericWarningMacro(<< "error (" << err << ") decompressing data");
    return nullptr;
  }
  unsigned int decompLength = this->Impl->ZStream.total_out;

  vtkNew<vtkPNGReader> reader;
  reader->SetMemoryBuffer(decompressed);
  reader->SetMemoryBufferLength(static_cast<vtkIdType>(decompLength));
  reader->Update();
  vtkNew<vtkImageData> data;
  data->ShallowCopy(reader->GetOutput());
  return data;
}

} // end namespace omf