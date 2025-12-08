/****************************************************************************
** Copyright 2019 The Open Group
** Copyright 2019 Bluware, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
****************************************************************************/

#include "DataBlock.h"
#include "OpenVDS/ValueConversion.h"
#include "CompilerDefines.h"

namespace OpenVDS
{

bool InitializeDataBlock(const DataBlockDescriptor &descriptor, DataBlock &dataBlock, Error &error)
{
  int32_t size[DataBlock::Dimensionality_Max];
  size[0] = descriptor.SizeX;
  size[1] = descriptor.SizeY;
  size[2] = descriptor.SizeZ;
  return InitializeDataBlock(descriptor.Format, descriptor.Components, (enum DataBlock::Dimensionality)(descriptor.Dimensionality), size, dataBlock, error);
}

bool InitializeDataBlock(VolumeDataFormat format, VolumeDataComponents components, enum DataBlock::Dimensionality dimensionality, const int32_t (&size)[DataBlock::Dimensionality_Max], DataBlock &dataBlock, Error &error)
{
  dataBlock.Components = components;
  dataBlock.Format = format;
  dataBlock.Dimensionality = dimensionality;

  switch(dimensionality)
  {
  case 1:
    dataBlock.Size[0] = size[0];
    dataBlock.Size[1] = 1;
    dataBlock.Size[2] = 1;
    dataBlock.Size[3] = 1;
    break;
  case 2:
    dataBlock.Size[0] = size[0];
    dataBlock.Size[1] = size[1];
    dataBlock.Size[2] = 1;
    dataBlock.Size[3] = 1;
    break;
  case 3:
    dataBlock.Size[0] = size[0];
    dataBlock.Size[1] = size[1];
    dataBlock.Size[2] = size[2];
    dataBlock.Size[3] = 1;
    break;
  default:
    error.string = "Serialized datablock has illegal dimensionality";
    error.code = -1;
    return false;
  }

  dataBlock.AllocatedSize[0] = (format == VolumeDataFormat::Format_1Bit) ? ((dataBlock.Size[0] * components) + 7) / 8 : GetAllocatedByteSizeForSize(dataBlock.Size[0]);
  dataBlock.AllocatedSize[1] = GetAllocatedByteSizeForSize(dataBlock.Size[1]);
  dataBlock.AllocatedSize[2] = GetAllocatedByteSizeForSize(dataBlock.Size[2]);
  dataBlock.AllocatedSize[3] = GetAllocatedByteSizeForSize(dataBlock.Size[3]);
  dataBlock.Pitch[0] = 1;
  for (int i = 1; i < DataBlock::Dimensionality_Max; i++)
  {
    dataBlock.Pitch[i] = dataBlock.Pitch[i - 1] * dataBlock.AllocatedSize[i - 1];
  }

  uint64_t allocatedByteSize = dataBlock.AllocatedSize[0];

  for (int i = 1; i < DataBlock::Dimensionality_Max; i++)
  {
    allocatedByteSize *= dataBlock.AllocatedSize[i];
  }

  if (allocatedByteSize * GetElementSize(dataBlock.Format, dataBlock.Components) > 0x7FFFFFFF)
  {
    char buffer[4096];
    snprintf(buffer, sizeof(buffer), "Datablock is too big (%d x %d x %d x %d x %d bytes)", dataBlock.AllocatedSize[0], dataBlock.AllocatedSize[1], dataBlock.AllocatedSize[2], dataBlock.AllocatedSize[3], GetElementSize(dataBlock.Format, dataBlock.Components));
    error.string = buffer;
    error.code = -1;
    return false;
  }
  return true;
}

const int MIN_MEMCPY = 4096;
template <unsigned int IVAL> struct FIND_SHIFT    { enum { RET = 1 + FIND_SHIFT<IVAL/2>::RET }; };
template <>                  struct FIND_SHIFT<1> { enum { RET = 0 }; };
template <>                  struct FIND_SHIFT<0> { enum { RET = 0 }; };

int32_t CombineAndReduceDimensions (int32_t (&sourceSize  )[DataBlock::Dimensionality_Max],
                                    int32_t (&sourceOffset)[DataBlock::Dimensionality_Max],
                                    int32_t (&targetSize  )[DataBlock::Dimensionality_Max],
                                    int32_t (&targetOffset)[DataBlock::Dimensionality_Max],
                                    int32_t (&overlapSize )[DataBlock::Dimensionality_Max],
                                    const int32_t (&origSourceSize  )[Dimensionality_Max],
                                    const int32_t (&origSourceOffset)[Dimensionality_Max],
                                    const int32_t (&origTargetSize  )[Dimensionality_Max],
                                    const int32_t (&origTargetOffset)[Dimensionality_Max],
                                    const int32_t (&origOverlapSize )[Dimensionality_Max])
{
    int32_t tmpSourceSize  [Dimensionality_Max] = {origSourceSize  [0],origSourceSize  [1],origSourceSize  [2],origSourceSize  [3],origSourceSize  [4],origSourceSize  [5]};
    int32_t tmpSourceOffset[Dimensionality_Max] = {origSourceOffset[0],origSourceOffset[1],origSourceOffset[2],origSourceOffset[3],origSourceOffset[4],origSourceOffset[5]};
    int32_t tmpTargetSize  [Dimensionality_Max] = {origTargetSize  [0],origTargetSize  [1],origTargetSize  [2],origTargetSize  [3],origTargetSize  [4],origTargetSize  [5]};
    int32_t tmpTargetOffset[Dimensionality_Max] = {origTargetOffset[0],origTargetOffset[1],origTargetOffset[2],origTargetOffset[3],origTargetOffset[4],origTargetOffset[5]};
    int32_t tmpOverlapSize [Dimensionality_Max] = {origOverlapSize [0],origOverlapSize [1],origOverlapSize [2],origOverlapSize [3],origOverlapSize [4],origOverlapSize [5]};

  // Combine dimensions where the overlap size is 1
  for (int32_t iCopyDimension = Dimensionality_Max - 1; iCopyDimension > 0; iCopyDimension--)
  {
    if (tmpOverlapSize[iCopyDimension] == 1)
    {
      tmpSourceOffset[iCopyDimension-1] += tmpSourceOffset[iCopyDimension] * tmpSourceSize[iCopyDimension-1];
      tmpTargetOffset[iCopyDimension-1] += tmpTargetOffset[iCopyDimension] * tmpTargetSize[iCopyDimension-1];
      tmpSourceOffset[iCopyDimension  ]  = 0;
      tmpTargetOffset[iCopyDimension  ]  = 0;

      tmpSourceSize[iCopyDimension-1] *= tmpSourceSize[iCopyDimension];
      tmpTargetSize[iCopyDimension-1] *= tmpTargetSize[iCopyDimension];
      tmpSourceSize[iCopyDimension  ]  = 1;
      tmpTargetSize[iCopyDimension  ]  = 1;
    }
  }

  assert(tmpOverlapSize[0] + tmpSourceOffset[0] <= tmpSourceSize[0] &&
    tmpOverlapSize[1] + tmpSourceOffset[1] <= tmpSourceSize[1] &&
    tmpOverlapSize[2] + tmpSourceOffset[2] <= tmpSourceSize[2] &&
    tmpOverlapSize[3] + tmpSourceOffset[3] <= tmpSourceSize[3] &&
    tmpOverlapSize[4] + tmpSourceOffset[4] <= tmpSourceSize[4] &&
    tmpOverlapSize[5] + tmpSourceOffset[5] <= tmpSourceSize[5] &&
    "Invalid Copy Parameters #1");


  assert(tmpOverlapSize[0] + tmpTargetOffset[0] <= tmpTargetSize[0] &&
    tmpOverlapSize[1] + tmpTargetOffset[1] <= tmpTargetSize[1] &&
    tmpOverlapSize[2] + tmpTargetOffset[2] <= tmpTargetSize[2] &&
    tmpOverlapSize[3] + tmpTargetOffset[3] <= tmpTargetSize[3] &&
    tmpOverlapSize[4] + tmpTargetOffset[4] <= tmpTargetSize[4] &&
    tmpOverlapSize[5] + tmpTargetOffset[5] <= tmpTargetSize[5] &&
    "Invalid Copy Parameters #1");

  int32_t nCopyDimensions = 0;

  // Reduce dimensions where the source and target size is 1 (and the offset is 0)
  for (int32_t dimension = 0; dimension < DataBlock::Dimensionality_Max; dimension++)
  {
    if(tmpSourceSize[dimension] == 1 && tmpTargetSize[dimension] == 1)
    {
      assert(tmpSourceOffset[dimension] == 0);
      assert(tmpTargetOffset[dimension] == 0);
      assert(tmpOverlapSize [dimension] == 1);
      continue;
    }

    sourceOffset[nCopyDimensions] = tmpSourceOffset[dimension];
    targetOffset[nCopyDimensions] = tmpTargetOffset[dimension];

    sourceSize[nCopyDimensions] = tmpSourceSize[dimension];
    targetSize[nCopyDimensions] = tmpTargetSize[dimension];

    overlapSize[nCopyDimensions] = tmpOverlapSize[dimension];

    nCopyDimensions++;
  }

  assert(nCopyDimensions <= DataBlock::Dimensionality_Max && "Invalid Copy Parameters #4");

  // Further combine inner dimensions if possible, to minimize number of copy invocations
  int32_t nCombineDimensions = nCopyDimensions - 1;

  for (int iCombineDimension = 0; iCombineDimension < nCombineDimensions; iCombineDimension++)
  {
    if (overlapSize[0] != sourceSize[0] || sourceSize[0] != targetSize[0])
    {
      break;
    }

    assert(sourceOffset[0] == 0 && "Invalid Copy Parameters #2");
    assert(targetOffset[0] == 0 && "Invalid Copy Parameters #3");

    sourceOffset[0] = sourceOffset[1] * sourceSize[0];
    targetOffset[0] = targetOffset[1] * targetSize[0];

    sourceSize [0] *= sourceSize [1];
    targetSize [0] *= targetSize [1];
    overlapSize[0] *= overlapSize[1];

    for (int dimension = 1; dimension < nCopyDimensions - 1; dimension++)
    {
      sourceOffset[dimension] = sourceOffset[dimension+1];
      targetOffset[dimension] = targetOffset[dimension+1];

      sourceSize [dimension] = sourceSize [dimension+1];
      targetSize [dimension] = targetSize [dimension+1];
      overlapSize[dimension] = overlapSize[dimension+1];
    }

    nCopyDimensions -= 1;
  }

  // Reset remaining dimensions
  for(int32_t dimension = nCopyDimensions; dimension < DataBlock::Dimensionality_Max; dimension++)
  {
    sourceOffset[dimension] = 0;
    targetOffset[dimension] = 0;

    sourceSize [dimension] = 1;
    targetSize [dimension] = 1;
    overlapSize[dimension] = 1;
  }

  return nCopyDimensions;
}

static force_inline void CopyBits(void* target, int64_t targetBit, const void* source, int64_t sourceBit, int32_t bits)
{
  while(bits--)
  {
    WriteElement(reinterpret_cast<bool *>(target), targetBit++, ReadElement(reinterpret_cast<const bool *>(source), sourceBit++));
  }
}

template <typename T>
static force_inline void CopyBytesT(T* __restrict target, const T* __restrict source, int32_t size)
{
  if (size >= MIN_MEMCPY)
  {
    memcpy (target, source, size_t(size));
  }
  else
  {
    int32_t nBigElements = size >> CALC_BIT_SHIFT(sizeof(T));
    for (int32_t iBigElement = 0; iBigElement < nBigElements; iBigElement++)
    {
      target [iBigElement] = source [iBigElement];
    }
    int32_t nTail = size & ((int32_t) sizeof (T) - 1);
    if (nTail)
    {
      assert(nTail <= 7 && "Invalid Sample Size Remainder\n");
      const uint8_t *sourceTail = (const uint8_t *) (source + nBigElements);
      uint8_t *targetTail = (uint8_t *) (target + nBigElements);

      int32_t iTail = 0;
      switch (nTail)
      {
      case 7: targetTail[iTail] = sourceTail[iTail]; iTail++; FALLTHROUGH;
      case 6: targetTail[iTail] = sourceTail[iTail]; iTail++; FALLTHROUGH;
      case 5: targetTail[iTail] = sourceTail[iTail]; iTail++; FALLTHROUGH;
      case 4: targetTail[iTail] = sourceTail[iTail]; iTail++; FALLTHROUGH;
      case 3: targetTail[iTail] = sourceTail[iTail]; iTail++; FALLTHROUGH;
      case 2: targetTail[iTail] = sourceTail[iTail]; iTail++; FALLTHROUGH;
      case 1: targetTail[iTail] = sourceTail[iTail]; 
      }
    }
  }
}

static force_inline void CopyBytes(void* target, const void* source, int32_t size)
{
  if (size >= int32_t(sizeof (int64_t)) && !((intptr_t) source & (sizeof (int64_t)-1)) && !((intptr_t) target & (sizeof (int64_t)-1)))
    CopyBytesT ((int64_t*) target, (int64_t*) source, size);
  else if (size >= int32_t(sizeof (int32_t)) && !((intptr_t) source & (sizeof (int32_t)-1)) && !((intptr_t) target & (sizeof (int32_t)-1)))
    CopyBytesT ((int32_t*) target, (int32_t*) source, size);
  else if (size >= int32_t(sizeof (int16_t)) && !((intptr_t) source & (sizeof (int16_t)-1)) && !((intptr_t) target & (sizeof (int16_t)-1)))
    CopyBytesT ((int16_t*) target, (int16_t*) source, size);
  else
    CopyBytesT ((int8_t*) target, (int8_t*) source, size);
}


template<typename T, bool is1Bit>
static void DispatchBlockCopy2(void *target, const int32_t (&targetOffset)[DataBlock::Dimensionality_Max], const int32_t (&targetSize)[DataBlock::Dimensionality_Max],
                              void const *source, const int32_t (&sourceOffset)[DataBlock::Dimensionality_Max], const int32_t (&sourceSize)[DataBlock::Dimensionality_Max],
                              const int32_t (&overlapSize) [DataBlock::Dimensionality_Max])
{
  int64_t sourceLocalBaseSize = ((((int64_t)sourceOffset[3] * sourceSize[2] + sourceOffset[2]) * sourceSize[1] + sourceOffset[1]) * sourceSize[0] + sourceOffset[0]) * (int64_t)sizeof(T);
  int64_t targetLocalBaseSize = ((((int64_t)targetOffset[3] * targetSize[2] + targetOffset[2]) * targetSize[1] + targetOffset[1]) * targetSize[0] + targetOffset[0]) * (int64_t)sizeof(T);
  const uint8_t* sourceLocalBase = reinterpret_cast<const uint8_t*>(source) + sourceLocalBaseSize;
  uint8_t* targetLocalBase = reinterpret_cast<uint8_t*>(target) + targetLocalBaseSize;

  for (int dimension3 = 0; dimension3 < overlapSize[3]; dimension3++)
  {
    for (int dimension2 = 0; dimension2 < overlapSize[2]; dimension2++)
    {
      for (int dimension1 = 0; dimension1 < overlapSize[1]; dimension1++)
      {
        int64_t sourceLocal = (((int64_t)dimension3 * sourceSize[2] + dimension2) * sourceSize[1] + dimension1) * (int64_t)sourceSize[0] * (int64_t)sizeof(T);
        int64_t targetLocal = (((int64_t)dimension3 * targetSize[2] + dimension2) * targetSize[1] + dimension1) * (int64_t)targetSize[0] * (int64_t)sizeof(T);
        if (is1Bit)
        {
          CopyBits(target, targetLocalBaseSize + targetLocal, source, sourceLocalBaseSize + sourceLocal, overlapSize[0]);
        }
        else
        {
          CopyBytes(targetLocalBase + targetLocal, sourceLocalBase + sourceLocal, overlapSize[0] * (int32_t)sizeof(T));
        }
      }
    }
  }
}

void DispatchBlockCopy(VolumeDataFormat format,
                       void       *target, const int32_t (&targetOffset)[DataBlock::Dimensionality_Max], const int32_t (&targetSize)[DataBlock::Dimensionality_Max],
                       void const *source, const int32_t (&sourceOffset)[DataBlock::Dimensionality_Max], const int32_t (&sourceSize)[DataBlock::Dimensionality_Max],
                       const int32_t (&overlapSize) [DataBlock::Dimensionality_Max])
{
  switch(format)
  {
  case VolumeDataFormat::Format_1Bit:
    return DispatchBlockCopy2<uint8_t, true>(target, targetOffset, targetSize, source, sourceOffset, sourceSize, overlapSize);
  case VolumeDataFormat::Format_U8:
    return DispatchBlockCopy2<uint8_t, false>(target, targetOffset, targetSize, source, sourceOffset, sourceSize, overlapSize);
  case VolumeDataFormat::Format_U16:
    return DispatchBlockCopy2<uint16_t, false>(target, targetOffset, targetSize, source, sourceOffset, sourceSize, overlapSize);
  case VolumeDataFormat::Format_R32:
    return DispatchBlockCopy2<float, false>(target, targetOffset, targetSize, source, sourceOffset, sourceSize, overlapSize);
  case VolumeDataFormat::Format_U32:
    return DispatchBlockCopy2<uint32_t, false>(target, targetOffset, targetSize, source, sourceOffset, sourceSize, overlapSize);
  case VolumeDataFormat::Format_R64:
    return DispatchBlockCopy2<double, false>(target, targetOffset, targetSize, source, sourceOffset, sourceSize, overlapSize);
  case VolumeDataFormat::Format_U64:
    return DispatchBlockCopy2<uint64_t, false>(target, targetOffset, targetSize, source, sourceOffset, sourceSize, overlapSize);
  case VolumeDataFormat::Format_Any:
    return DispatchBlockCopy2<uint8_t, false>(target, targetOffset, targetSize, source, sourceOffset, sourceSize, overlapSize);
  }
}

template <typename T>
void
FixupBorder(T * buffer, int offset, const int (&pitch)[DataBlock::Dimensionality_Max], BorderMode borderMode, const int (&borderMin)[DataBlock::Dimensionality_Max], const int (&borderSize)[DataBlock::Dimensionality_Max], const int (&fixupSize)[DataBlock::Dimensionality_Max])
{
  for (int dim3 = 0; dim3 < borderSize[3]; dim3++)
  {
    for (int dim2 = 0; dim2 < borderSize[2]; dim2++)
    {
      for (int dim1 = 0; dim1 < borderSize[1]; dim1++)
      {
        for (int dim0 = 0; dim0 < borderSize[0]; dim0++)
        {
          int
            writeOffset = dim0 * pitch[0] +
                          dim1 * pitch[1] + 
                          dim2 * pitch[2] + 
                          dim3 * pitch[3] + offset;

          if(borderMode == BorderMode::Clear || borderMode == BorderMode::None)
          {
            WriteElement(buffer, writeOffset, T());
            continue;
          }

          int
            writePos[DataBlock::Dimensionality_Max],
            readPos[DataBlock::Dimensionality_Max];

          writePos[0] = borderMin[0] + dim0;
          writePos[1] = borderMin[1] + dim1;
          writePos[2] = borderMin[2] + dim2;
          writePos[3] = borderMin[3] + dim3;


          if(borderMode == BorderMode::Mirror)
          {
            for (int i = 0; i < DataBlock::Dimensionality_Max; i++)
            {
              int
                newPos = abs(writePos[i]) % (2 * fixupSize[i]);

              if (newPos >= fixupSize[i])
              {
                newPos = 2 * fixupSize[i] - newPos - 1;
              }

              readPos[i] = newPos;
            }
          }
          else //if(borderMode == BorderMode::Repeat)
          {
            assert(borderMode == BorderMode::Repeat);
            for (int i = 0; i < DataBlock::Dimensionality_Max; i++)
            {
              int
                newPos = writePos[i];

              if (newPos >= fixupSize[i])
              {
                newPos = fixupSize[i] - 1;
              }
              else if (newPos < 0)
              {
                newPos = 0;
              }

              readPos[i] = newPos;
            }
          }

          int
            readOffset = (readPos[0] - borderMin[0]) * pitch[0] +
                         (readPos[1] - borderMin[1]) * pitch[1] + 
                         (readPos[2] - borderMin[2]) * pitch[2] + 
                         (readPos[3] - borderMin[3]) * pitch[3] + offset;

          WriteElement(buffer, writeOffset, ReadElement(buffer, readOffset));
        }
      }
    }
  }
}

void FixupBorder(DataBlock const &dataBlock, void *buffer, VolumeDataFormat format, VolumeDataComponents components, BorderMode borderMode, const int (&borderNegativeRadius)[6], const int (&borderPositiveRadius)[6], const int (&layoutMin)[6], const int (&layoutSize)[6], const int (&layoutDimension)[DataBlock::Dimensionality_Max])
{
  int validOffset = 0; // Offset in elements of valid area inside datablock

  int pitch[DataBlock::Dimensionality_Max];
  int fixupMin[DataBlock::Dimensionality_Max];
  int fixupSize[DataBlock::Dimensionality_Max];
  int validMin[DataBlock::Dimensionality_Max];
  int validSize[DataBlock::Dimensionality_Max];

  for(int dataBlockDimension = 0; dataBlockDimension < DataBlock::Dimensionality_Max; dataBlockDimension++)
  {
    pitch[dataBlockDimension] = dataBlock.Pitch[dataBlockDimension];
    if(format == VolumeDataFormat::Format_1Bit && dataBlockDimension > 0)
    {
      pitch[dataBlockDimension] *= 8;
    }

    // Get fixup min/size
    if(layoutDimension[dataBlockDimension] >= 0)
    {
      assert(layoutDimension[dataBlockDimension] < 6);
      fixupMin[dataBlockDimension] = layoutMin[layoutDimension[dataBlockDimension]];
      fixupSize[dataBlockDimension] = layoutSize[layoutDimension[dataBlockDimension]];
    }
    else
    {
      fixupMin[dataBlockDimension] = 0;
      fixupSize[dataBlockDimension] = 1;
    }

    // Calculate valid min/size
    if(fixupMin[dataBlockDimension] < 0)
    {
      validMin[dataBlockDimension] = 0;
      validOffset += -layoutMin[layoutDimension[dataBlockDimension]] * pitch[dataBlockDimension];
    }
    else
    {
      validMin[dataBlockDimension] = fixupMin[dataBlockDimension];
    }

    validSize[dataBlockDimension] = std::min(fixupSize[dataBlockDimension], fixupMin[dataBlockDimension] + dataBlock.Size[dataBlockDimension]) - validMin[dataBlockDimension];
  }

  for(int fixupDimension = 0; fixupDimension < DataBlock::Dimensionality_Max; /* fixupDimension is increased at the end if no fixup was needed for this dimension */)
  {
    bool
      needsFixup = false;

    int borderMin[DataBlock::Dimensionality_Max];
    int borderSize[DataBlock::Dimensionality_Max];

    if(fixupMin[fixupDimension] < validMin[fixupDimension])
    {
      for(int dimension = 0; dimension < DataBlock::Dimensionality_Max; dimension++)
      {
        if(dimension == fixupDimension)
        {
          borderMin[dimension] = fixupMin[dimension];
          borderSize[dimension] = validMin[dimension] - fixupMin[dimension];
        }
        else
        {
          borderMin[dimension] = validMin[dimension];
          borderSize[dimension] = validSize[dimension];
        }
      }

      needsFixup = true;
    }
    else if(dataBlock.Size[fixupDimension] > validSize[fixupDimension])
    {
      assert(fixupMin[fixupDimension] == validMin[fixupDimension] && "We should already have fixed up the negative side");
      for(int dimension = 0; dimension < DataBlock::Dimensionality_Max; dimension++)
      {
        if(dimension == fixupDimension)
        {
          borderMin[dimension] = fixupSize[fixupDimension];
          borderSize[dimension] = dataBlock.Size[fixupDimension] - validSize[fixupDimension];
        }
        else
        {
          borderMin[dimension] = validMin[dimension];
          borderSize[dimension] = validSize[dimension];
        }
      }

      needsFixup = true;
    }

    if(needsFixup)
    {
      int borderOffset = validOffset + (borderMin[fixupDimension] - validMin[fixupDimension]) * pitch[fixupDimension];

      switch(components)
      {
      case VolumeDataComponents::Components_1:
        switch(format)
        {
        case VolumeDataFormat::Format_1Bit:
          FixupBorder((bool *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U8:
          FixupBorder((uint8_t *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U16:
          FixupBorder((uint16_t *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_R32:
          FixupBorder((float *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U32:
          FixupBorder((uint32_t *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_R64:
          FixupBorder((double *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U64:
          FixupBorder((uint64_t *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        default:
          throw std::runtime_error("Illegal datablock format");
        }
        break;

      case VolumeDataComponents::Components_2:
        switch(format)
        {
        case VolumeDataFormat::Format_1Bit:
          assert(0 && "Fixup is not implemented for multi-component 1-bit data");
          break;
        case VolumeDataFormat::Format_U8:
          FixupBorder((Vector<uint8_t,2> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U16:
          FixupBorder((Vector<uint16_t,2> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_R32:
          FixupBorder((Vector<float,2> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U32:
          FixupBorder((Vector<uint32_t,2> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_R64:
          FixupBorder((Vector<double,2> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U64:
          FixupBorder((Vector<uint64_t,2> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        default:
          throw std::runtime_error("Illegal datablock format");
        }
        break;

      case VolumeDataComponents::Components_4:
        switch(format)
        {
        case VolumeDataFormat::Format_1Bit:
          assert(0 && "Fixup is not implemented for multi-component 1-bit data");
          break;
        case VolumeDataFormat::Format_U8:
          FixupBorder((Vector<uint8_t,4> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U16:
          FixupBorder((Vector<uint16_t,4> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_R32:
          FixupBorder((Vector<float,4> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U32:
          FixupBorder((Vector<uint32_t,4> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_R64:
          FixupBorder((Vector<double,4> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        case VolumeDataFormat::Format_U64:
          FixupBorder((Vector<uint64_t,4> *)buffer, borderOffset, pitch, borderMode, borderMin, borderSize, fixupSize);
          break;
        default:
          throw std::runtime_error("Illegal datablock format");
        }
        break;

      default:
        throw std::runtime_error("Illegal number of datablock components");
      }

      // Increase valid size
      if(validMin[fixupDimension] > borderMin[fixupDimension])
      {
        assert(validMin[fixupDimension] - borderMin[fixupDimension] == borderSize[fixupDimension]);
        validMin[fixupDimension] = borderMin[fixupDimension];
        validOffset -= borderSize[fixupDimension] * pitch[fixupDimension];
      }
      validSize[fixupDimension] += borderSize[fixupDimension];
    }
    else
    {
      // This dimension is valid, go to next dimension
      assert(validSize[fixupDimension] == dataBlock.Size[fixupDimension]);
      fixupDimension++;
    }
  }
}

template <typename T>
static void
GenerateLOD(T *targetBuffer, const T * sourceBuffer, int targetModuloY, int targetModuloZ, int sourceModuloY, int sourceModuloZ, int sizeX, int sizeY, int sizeZ, int fullResolutionDimension)
{
  int XFactor = fullResolutionDimension == 0 ? 1 : 2;
  int YFactor = fullResolutionDimension == 1 ? 1 : 2;
  int ZFactor = fullResolutionDimension == 2 ? 1 : 2;

  for (int iZ = 0; iZ < sizeZ; iZ++)
  {
    for (int iY = 0; iY < sizeY; iY++)
    {
      T *target = targetBuffer + iY * targetModuloY + iZ * targetModuloZ;

      const T *source = sourceBuffer + iY * sourceModuloY * YFactor + iZ * sourceModuloZ * ZFactor;

      for (int iX = 0; iX < sizeX; iX++)
      {
        target[iX] = source[iX * XFactor];
      }
    }
  }
}

template <bool READNEXTX, bool READNEXTY, bool READNEXTZ, typename T>
static void
GenerateLODExcludingNoValue(T *targetBuffer, const T * sourceBuffer, int targetModuloY, int targetModuloZ, int sourceModuloY, int sourceModuloZ, int sizeX, int sizeY, int sizeZ, T noValue, int fullResolutionDimension)
{
  int XFactor = fullResolutionDimension == 0 ? 1 : 2;
  int YFactor = fullResolutionDimension == 1 ? 1 : 2;
  int ZFactor = fullResolutionDimension == 2 ? 1 : 2;

  for (int iZ = 0; iZ < sizeZ; iZ++)
  {
    for (int iY = 0; iY < sizeY; iY++)
    {
      T *target = targetBuffer + iY * targetModuloY + iZ * targetModuloZ;

      const T *source = sourceBuffer + iY * sourceModuloY * YFactor + iZ * sourceModuloZ * ZFactor;

      for (int iX = 0; iX < sizeX; iX++)
      {
        T value = source[iX * XFactor];

        if (                          READNEXTX && value == noValue) value = source[iX * XFactor + 1                                ];
        if (             READNEXTY              && value == noValue) value = source[iX * XFactor     + sourceModuloY                ];
        if (             READNEXTY && READNEXTX && value == noValue) value = source[iX * XFactor + 1 + sourceModuloY                ];
        if (READNEXTZ                           && value == noValue) value = source[iX * XFactor                     + sourceModuloZ];
        if (READNEXTZ              && READNEXTX && value == noValue) value = source[iX * XFactor + 1                 + sourceModuloZ];
        if (READNEXTZ && READNEXTY              && value == noValue) value = source[iX * XFactor     + sourceModuloY + sourceModuloZ];
        if (READNEXTZ && READNEXTY && READNEXTX && value == noValue) value = source[iX * XFactor + 1 + sourceModuloY + sourceModuloZ];

        target[iX] = value;
      }
    }
  }
}

template<bool READNEXTX, bool READNEXTY, bool READNEXTZ>
static void
GenerateLODExcludingNoValue(void *targetBuffer, const void * sourceBuffer, int targetModuloY, int targetModuloZ, int sourceModuloY, int sourceModuloZ, int sizeX, int sizeY, int sizeZ, float noValue, int fullResolutionDimension, VolumeDataFormat format)
{
  switch(format)
  {
  default: throw std::runtime_error("Illegal format");

  case VolumeDataFormat::Format_U8:  return GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>((uint8_t  *)targetBuffer, (const uint8_t  *)sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, (uint8_t )0, fullResolutionDimension);
  case VolumeDataFormat::Format_U16: return GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>((uint16_t *)targetBuffer, (const uint16_t *)sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, (uint16_t)0, fullResolutionDimension);
  case VolumeDataFormat::Format_U32: return GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>((uint32_t *)targetBuffer, (const uint32_t *)sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, (uint32_t)0, fullResolutionDimension);
  case VolumeDataFormat::Format_U64: return GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>((uint64_t *)targetBuffer, (const uint64_t *)sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, (uint64_t)0, fullResolutionDimension);
  case VolumeDataFormat::Format_R32: return GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>((float    *)targetBuffer, (const float    *)sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, (float )noValue, fullResolutionDimension);
  case VolumeDataFormat::Format_R64: return GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>((double   *)targetBuffer, (const double   *)sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, (double)noValue, fullResolutionDimension);
  }
}

template<bool READNEXTX, bool READNEXTY, bool READNEXTZ>
static void
GenerateLODExcludingNoValue(void *targetBuffer, const void * sourceBuffer, int targetModuloY, int targetModuloZ, int sourceModuloY, int sourceModuloZ, int sizeX, int sizeY, int sizeZ, float noValue, int fullResolutionDimension, VolumeDataFormat format, bool lastValidX, bool lastValidY, bool lastValidZ, int elementSize)
{
  int XFactor = fullResolutionDimension == 0 ? 1 : 2;
  int YFactor = fullResolutionDimension == 1 ? 1 : 2;
  int ZFactor = fullResolutionDimension == 2 ? 1 : 2;

  if (READNEXTZ && ZFactor == 1)
  {
    GenerateLODExcludingNoValue<READNEXTX, READNEXTY, false>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ , noValue, fullResolutionDimension, format, lastValidX, lastValidY, lastValidZ, elementSize);
  }
  else if (READNEXTY && YFactor == 1)
  {
    GenerateLODExcludingNoValue<READNEXTX, false, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ , noValue, fullResolutionDimension, format, lastValidX, lastValidY, lastValidZ, elementSize);
  }
  else if (READNEXTX && XFactor == 1)
  {
    GenerateLODExcludingNoValue<false, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ , noValue, fullResolutionDimension, format, lastValidX, lastValidY, lastValidZ, elementSize);
  }
  else if(READNEXTZ && !lastValidZ)
  {
    GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ - 1, noValue, fullResolutionDimension, format, lastValidX, lastValidY, true, elementSize);
    GenerateLODExcludingNoValue<READNEXTX, READNEXTY, false>((uint8_t *)targetBuffer + targetModuloZ * (sizeZ - 1) * elementSize, (uint8_t *)sourceBuffer + sourceModuloZ * (sizeZ - 1) * elementSize * ZFactor, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, 1, noValue, fullResolutionDimension, format, lastValidX, lastValidY, true, elementSize);
  }
  else if(READNEXTY && !lastValidY)
  {
    GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY - 1, sizeZ, noValue, fullResolutionDimension, format, lastValidX, true, lastValidZ, elementSize);
    GenerateLODExcludingNoValue<READNEXTX, false, READNEXTZ>((uint8_t *)targetBuffer + targetModuloY * (sizeY - 1) * elementSize, (uint8_t *)sourceBuffer + sourceModuloY * (sizeY - 1) * elementSize * YFactor, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, 1, sizeZ, noValue, fullResolutionDimension, format, lastValidX, true, lastValidZ, elementSize);
  }
  else if(READNEXTX && !lastValidX)
  {
    GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX - 1, sizeY, sizeZ, noValue, fullResolutionDimension, format, true, lastValidY, lastValidZ, elementSize);
    GenerateLODExcludingNoValue<false, READNEXTY, READNEXTZ>((uint8_t *)targetBuffer + (sizeX - 1) * elementSize, (uint8_t *)sourceBuffer + (sizeX - 1) * elementSize * XFactor, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, 1, sizeY, sizeZ, noValue, fullResolutionDimension, format, true, lastValidY, lastValidZ, elementSize);
  }
  else
  {
    GenerateLODExcludingNoValue<READNEXTX, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, noValue, fullResolutionDimension, format);
  }
}

template<bool READNEXTX, bool READNEXTY, bool READNEXTZ>
static void
GenerateLOD1Bit(uint8_t *targetBuffer, const uint8_t * sourceBuffer, int targetModuloY, int targetModuloZ, int sourceModuloY, int sourceModuloZ, int sizeX, int sizeY, int sizeZ, int targetOffsetX, int sourceOffsetX, int fullResolutionDimension)
{
  int XFactor = fullResolutionDimension == 0 ? 1 : 2;
  int YFactor = fullResolutionDimension == 1 ? 1 : 2;
  int ZFactor = fullResolutionDimension == 2 ? 1 : 2;

  for (int iZ = 0; iZ < sizeZ; iZ++)
  {
    for (int iY = 0; iY < sizeY; iY++)
    {
      uint8_t *target = targetBuffer + iY * targetModuloY + iZ * targetModuloZ;

      const uint8_t *source = sourceBuffer + iY * sourceModuloY * YFactor + iZ * sourceModuloZ * ZFactor;

      for (int iX = 0; iX < sizeX; iX++)
      {
        int
          iDstX = iX + targetOffsetX,
          bDstBit = 1 << (iDstX & 7),
          iSrcX0 = iX * XFactor + sourceOffsetX,
          iSrcX1 = iX * XFactor + 1 + sourceOffsetX,
          bSrcBit0 = 1 << (iSrcX0 & 7),
          bSrcBit1 = 1 << (iSrcX1 & 7);

        iDstX >>= 3;
        iSrcX0 >>= 3;
        iSrcX1 >>= 3;

        // Clear dest bit
        target[iDstX] &= ~bDstBit;

        bool
          isSet = (source[iSrcX0] & bSrcBit0);

        if (                          READNEXTX && !isSet) isSet = (source[iSrcX1                                ] & bSrcBit1) != 0;
        if (             READNEXTY              && !isSet) isSet = (source[iSrcX0 + sourceModuloY                ] & bSrcBit0) != 0;
        if (             READNEXTY && READNEXTX && !isSet) isSet = (source[iSrcX1 + sourceModuloY                ] & bSrcBit1) != 0;
        if (READNEXTZ                           && !isSet) isSet = (source[iSrcX0                 + sourceModuloZ] & bSrcBit0) != 0;
        if (READNEXTZ              && READNEXTX && !isSet) isSet = (source[iSrcX1                 + sourceModuloZ] & bSrcBit1) != 0;
        if (READNEXTZ && READNEXTY              && !isSet) isSet = (source[iSrcX0 + sourceModuloY + sourceModuloZ] & bSrcBit0) != 0;
        if (READNEXTZ && READNEXTY && READNEXTX && !isSet) isSet = (source[iSrcX1 + sourceModuloY + sourceModuloZ] & bSrcBit1) != 0;

        if (isSet)
        {
          target[iDstX] |= bDstBit;
        }
      }
    }
  }
}

template<bool READNEXTX, bool READNEXTY, bool READNEXTZ>
static void
GenerateLOD1Bit(uint8_t *targetBuffer, const uint8_t * sourceBuffer, int targetModuloY, int targetModuloZ, int sourceModuloY, int sourceModuloZ, int sizeX, int sizeY, int sizeZ, int targetOffsetX, int sourceOffsetX, int fullResolutionDimension, bool lastValidY, bool lastValidZ)
{
  int XFactor = fullResolutionDimension == 0 ? 1 : 2;
  int YFactor = fullResolutionDimension == 1 ? 1 : 2;
  int ZFactor = fullResolutionDimension == 2 ? 1 : 2;

  if (READNEXTZ && ZFactor == 1)
  {
    GenerateLOD1Bit<READNEXTX, READNEXTY, false>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension, lastValidY, lastValidZ);
  }
  else if (READNEXTY && YFactor == 1)
  {
    GenerateLOD1Bit<READNEXTX, false, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension, lastValidY, lastValidZ);
  }
  else if (READNEXTX && XFactor == 1)
  {
    GenerateLOD1Bit<false, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension, lastValidY, lastValidZ);
  }
  else if(READNEXTZ && !lastValidZ)
  {
    GenerateLOD1Bit<READNEXTX, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ - 1, targetOffsetX, sourceOffsetX, fullResolutionDimension, lastValidY, true);
    GenerateLOD1Bit<READNEXTX, READNEXTY, false>((uint8_t *)targetBuffer + targetModuloZ * (sizeZ - 1), (uint8_t *)sourceBuffer + sourceModuloZ * (sizeZ - 1) * ZFactor, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, 1, targetOffsetX, sourceOffsetX, fullResolutionDimension, lastValidY, true);
  }
  else if(READNEXTY && !lastValidY)
  {
    GenerateLOD1Bit<READNEXTX, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY - 1, sizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension, true, lastValidZ);
    GenerateLOD1Bit<READNEXTX, false, READNEXTZ>((uint8_t *)targetBuffer + targetModuloY * (sizeY - 1), (uint8_t *)sourceBuffer + sourceModuloY * (sizeY - 1) * YFactor, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, 1, sizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension, true, lastValidZ);
  }
  else
  {
    GenerateLOD1Bit<READNEXTX, READNEXTY, READNEXTZ>(targetBuffer, sourceBuffer, targetModuloY, targetModuloZ, sourceModuloY, sourceModuloZ, sizeX, sizeY, sizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension);
  }
}

void DownSampleAndCopyRegion(DataBlock const &targetDataBlock,
                             DataBlock const &sourceDataBlock,
                             void *targetBuffer, const void *sourceBuffer,
                             int targetOffsetX, int targetOffsetY, int targetOffsetZ,
                             int targetSizeX,   int targetSizeY,   int targetSizeZ,
                             int sourceOffsetX, int sourceOffsetY, int sourceOffsetZ,
                             float rNoValue, int fullResolutionDimension)
{
  assert(targetDataBlock.Format == sourceDataBlock.Format && targetDataBlock.Components == sourceDataBlock.Components);
  assert((targetDataBlock.Components == 1 || targetDataBlock.Format == VolumeDataFormat::Format_U8) && "Only U8 format supports LOD generation for multi component data");

  int XFactor = fullResolutionDimension == 0 ? 1 : 2;
  int YFactor = fullResolutionDimension == 1 ? 1 : 2;
  int ZFactor = fullResolutionDimension == 2 ? 1 : 2;

  // Check if read outside valid area is possible
  bool lastValidX = sourceOffsetX + targetSizeX * XFactor <= sourceDataBlock.Size[0];
  bool lastValidY = sourceOffsetY + targetSizeY * YFactor <= sourceDataBlock.Size[1];
  bool lastValidZ = sourceOffsetZ + targetSizeZ * ZFactor <= sourceDataBlock.Size[2];

  int
    elementSize = GetElementSize(sourceDataBlock);

  int
    sourceAllocatedSizeX = sourceDataBlock.AllocatedSize[0],
    sourceAllocatedSizeY = sourceDataBlock.AllocatedSize[1],
    targetAllocatedSizeX = targetDataBlock.AllocatedSize[0],
    targetAllocatedSizeY = targetDataBlock.AllocatedSize[1];

  if (targetDataBlock.Format == VolumeDataFormat::Format_1Bit)
  {
    sourceBuffer = (uint8_t *)sourceBuffer + (sourceOffsetY * sourceAllocatedSizeX + sourceOffsetZ * sourceAllocatedSizeX * sourceAllocatedSizeY) * elementSize;
    targetBuffer = (uint8_t *)targetBuffer + (targetOffsetY * targetAllocatedSizeX + targetOffsetZ * targetAllocatedSizeX * targetAllocatedSizeY) * elementSize;

    if(sourceDataBlock.Dimensionality == 1)
    {
      GenerateLOD1Bit<true, false, false>((uint8_t *)targetBuffer, (uint8_t *)sourceBuffer,
        targetAllocatedSizeX, targetAllocatedSizeX * targetAllocatedSizeY,
        sourceAllocatedSizeX, sourceAllocatedSizeX * sourceAllocatedSizeY,
        targetSizeX, targetSizeY, targetSizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension, lastValidY, lastValidZ);
    }
    else if(sourceDataBlock.Dimensionality == 2)
    {
      GenerateLOD1Bit<true, true, false>((uint8_t *)targetBuffer, (uint8_t *)sourceBuffer, 
        targetAllocatedSizeX, targetAllocatedSizeX * targetAllocatedSizeY, 
        sourceAllocatedSizeX, sourceAllocatedSizeX * sourceAllocatedSizeY,
        targetSizeX, targetSizeY, targetSizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension, lastValidY, lastValidZ);
    }
    else
    {
      assert(sourceDataBlock.Dimensionality == 3);
      GenerateLOD1Bit<true, true, true>((uint8_t *)targetBuffer, (uint8_t *)sourceBuffer, 
        targetAllocatedSizeX, targetAllocatedSizeX * targetAllocatedSizeY, 
        sourceAllocatedSizeX, sourceAllocatedSizeX * sourceAllocatedSizeY,
        targetSizeX, targetSizeY, targetSizeZ, targetOffsetX, sourceOffsetX, fullResolutionDimension, lastValidY, lastValidZ);
    }
  }
  else
  {
    sourceBuffer = (uint8_t *)sourceBuffer + (sourceOffsetX + sourceOffsetY * sourceAllocatedSizeX + sourceOffsetZ * sourceAllocatedSizeX * sourceAllocatedSizeY) * elementSize;
    targetBuffer = (uint8_t *)targetBuffer + (targetOffsetX + targetOffsetY * targetAllocatedSizeX + targetOffsetZ * targetAllocatedSizeX * targetAllocatedSizeY) * elementSize;

    if(targetDataBlock.Format == VolumeDataFormat::Format_U8 && targetDataBlock.Components == 4)
    {
      GenerateLOD((uint32_t *)targetBuffer, (uint32_t *)sourceBuffer,
        targetAllocatedSizeX, targetAllocatedSizeX * targetAllocatedSizeY,
        sourceAllocatedSizeX, sourceAllocatedSizeX * sourceAllocatedSizeY,
        targetSizeX, targetSizeY, targetSizeZ, fullResolutionDimension);
    }
    else if(targetDataBlock.Format == VolumeDataFormat::Format_U8 && targetDataBlock.Components == 2)
    {
      GenerateLOD((uint16_t *)targetBuffer, (uint16_t *)sourceBuffer,
        targetAllocatedSizeX, targetAllocatedSizeX * targetAllocatedSizeY,
        sourceAllocatedSizeX, sourceAllocatedSizeX * sourceAllocatedSizeY,
        targetSizeX, targetSizeY, targetSizeZ, fullResolutionDimension);
    }
    else
    {
      if(sourceDataBlock.Dimensionality == 1)
      {
        GenerateLODExcludingNoValue<true, false, false>(targetBuffer, sourceBuffer,
          targetAllocatedSizeX, targetAllocatedSizeX * targetAllocatedSizeY,
          sourceAllocatedSizeX, sourceAllocatedSizeX * sourceAllocatedSizeY,
          targetSizeX, targetSizeY, targetSizeZ, rNoValue, fullResolutionDimension, targetDataBlock.Format, lastValidX, lastValidY, lastValidZ, elementSize);
      }
      else if(sourceDataBlock.Dimensionality == 2)
      {
        GenerateLODExcludingNoValue<true, true, false>(targetBuffer, sourceBuffer,
          targetAllocatedSizeX, targetAllocatedSizeX * targetAllocatedSizeY,
          sourceAllocatedSizeX, sourceAllocatedSizeX * sourceAllocatedSizeY,
          targetSizeX, targetSizeY, targetSizeZ, rNoValue, fullResolutionDimension, targetDataBlock.Format, lastValidX, lastValidY, lastValidZ, elementSize);
      }
      else
      {
        assert(sourceDataBlock.Dimensionality == 3);
        GenerateLODExcludingNoValue<true, true, true>(targetBuffer, sourceBuffer,
          targetAllocatedSizeX, targetAllocatedSizeX * targetAllocatedSizeY,
          sourceAllocatedSizeX, sourceAllocatedSizeX * sourceAllocatedSizeY,
          targetSizeX, targetSizeY, targetSizeZ, rNoValue, fullResolutionDimension, targetDataBlock.Format, lastValidX, lastValidY, lastValidZ, elementSize);
      }
    }
  }
}

}
