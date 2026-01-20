#include <OpenVDS/OpenVDS.h>
#include <OpenVDS/VolumeDataLayout.h>
#include <OpenVDS/VolumeDataAccess.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "colorbar.hpp"
#include "running_stat.hpp"

// disable error
// -Werror=missing-field-initializers
#ifdef __GNUC__
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.hpp"

#ifdef __GNUC__
# pragma GCC diagnostic pop
#endif

/*  thumbnailer parameters
    https://askubuntu.com/questions/1368910/how-to-create-custom-thumbnailers-for-nautilus-nemo-and-caja

Thumbnailers are chosen by examining a series of .thumbnailer files in `$PREFIX/share/thumbnailers`. Each is in a simple key-file format:

    [Thumbnailer Entry]
    TryExec=evince-thumbnailer
    Exec=evince-thumbnailer -s %s %u %o
    MimeType=application/pdf;application/x-bzpdf;application/x-gzpdf;

Entries:

    * Exec: Required. The command to execute the thumbnailer: 

      * %u URI of the file being thumbnailed
      * %i filename of file
      * %o filename of the thumbnail
      * %s is the maximum desired short dimension in pixels
      * %% is a literal percent character

    * MimeType: Required. A semicolon-separated list of supported MIME types.

*/

// #define DIMENSION_NAME()

const char * dimensionGroupName(OpenVDS::DimensionsND group, FILE *log) {
  switch(group) {
    case OpenVDS::Dimensions_01: return "01";
    case OpenVDS::Dimensions_02: return "02";
    case OpenVDS::Dimensions_03: return "03";
    case OpenVDS::Dimensions_12: return "12";
    case OpenVDS::Dimensions_13: return "13";
    case OpenVDS::Dimensions_23: return "23";
    case OpenVDS::Dimensions_012: return "012";
    case OpenVDS::Dimensions_013: return "013";
    case OpenVDS::Dimensions_023: return "023";
    case OpenVDS::Dimensions_123: return "123";
    default: fprintf(log, "Unexpected dimension.\n"); exit(1);
  }
}

int main(int argc, char *argv[])
{
  // open a log file
  FILE *log = fopen("/tmp/vds-thumbnailer.log", "w");

  // usage message if <2 parameters passed
  if (argc<3) {
    fprintf(log, "\nUsage: %s <document file> <thumbnail file to generate> [optional size in pixels]\n\n",
           argv[0]);
    exit(1);
  }

  std::string filename = argv[1];
  std::string thumbnail_file = argv[2];
  uint32_t sidePixels = argc>3 ? atoi(argv[3]) : 256;
  if (sidePixels<64 || sidePixels>1024) {
    fprintf(log, "Bad value for --size <thumbnail size> of '%s'.\n", argv[4]);
    exit(1);
  }

  fprintf(log, "Ran command %s\n", argv[0]);
  fprintf(log, "  %s\n", argv[1]);
  fprintf(log, "  %s\n", argv[2]);
  if (argc>3)
    fprintf(log, "  %s\n", argv[3]);
  fprintf(log, "\n");

  // open file as vds
  OpenVDS::Error error;
  OpenVDS::VDSHandle handle = OpenVDS::Open(filename, error);

  // check for errors
  if(error.code != 0)
  {
    std::cerr << "Could not open VDS: " << error.string << std::endl;
    exit(1);
  }
  OpenVDS::VolumeDataAccessManager volumeDataAccessManager = OpenVDS::GetAccessManager(handle);
  OpenVDS::VolumeDataLayout const *volumeDataLayout = volumeDataAccessManager.GetVolumeDataLayout();
  volumeDataLayout->GetChannelCount();
  volumeDataLayout->GetDimensionality();
  volumeDataLayout->GetAxisDescriptor(0);
  volumeDataLayout->GetChannelDescriptor(0);
  OpenVDS::VolumeDataLayoutDescriptor volumeDataLayoutDescriptor = volumeDataLayout->GetLayoutDescriptor();
  volumeDataLayoutDescriptor.GetBrickSize();
  volumeDataLayoutDescriptor.GetLODLevels();
  volumeDataLayoutDescriptor.GetFullResolutionDimension();
  volumeDataLayoutDescriptor.GetNegativeMargin();
  volumeDataLayoutDescriptor.GetPositiveMargin();

  bool hasSampleAxis{false};

  fprintf(log, "\nVolume axes / dimensions:\n");
  for (int dim=0; dim<volumeDataLayout->GetDimensionality(); ++dim) {
    OpenVDS::VolumeDataAxisDescriptor axisDescriptor = volumeDataLayout->GetAxisDescriptor(dim);
    if (!hasSampleAxis && !strcmp(axisDescriptor.GetName(),"Sample")) {
      hasSampleAxis = true;
    }
    fprintf(log, "  Dimension %d  %s  %f - %f (%d)\n", dim, axisDescriptor.GetName(),
            axisDescriptor.GetCoordinateMin(), axisDescriptor.GetCoordinateMax(), axisDescriptor.GetNumSamples());
  }
  int bestChannel = -1, bestLOD = -1;
  OpenVDS::DimensionsND bestDimensionGroup = OpenVDS::Dimensions_45;
  OpenVDS::VDSProduceStatus bestVDSProduceStatus = OpenVDS::VDSProduceStatus::Unavailable;
  fprintf(log, "\nLayer status:\n");
  for (auto channel=0; channel<volumeDataLayout->GetChannelCount(); ++channel) {
    if (!volumeDataLayout->IsChannelRenderable(channel)) {
      std::string channelName = (volumeDataLayout->GetChannelName(channel) && strlen(volumeDataLayout->GetChannelName(channel))) ? volumeDataLayout->GetChannelName(channel) : "UNNAMED";
      fprintf(log, "  Skipping channel #%d %s as unrenderable.\n", channel, channelName.c_str());
      continue;
    }
    std::string channelName = (volumeDataLayout->GetChannelName(channel) && strlen(volumeDataLayout->GetChannelName(channel))) ? volumeDataLayout->GetChannelName(channel) : "UNNAMED";
    fprintf(log, "  Channel #%d %s\n", channel, channelName.c_str());
    for (auto dimensionGroup : { OpenVDS::Dimensions_01,
                                 OpenVDS::Dimensions_02,
                                 OpenVDS::Dimensions_03,
                                 OpenVDS::Dimensions_12,
                                 OpenVDS::Dimensions_13,
                                 OpenVDS::Dimensions_23,
                                 OpenVDS::Dimensions_012,
                                 OpenVDS::Dimensions_013,
                                 OpenVDS::Dimensions_023 }) {
      fprintf(log, "    Dimension_%s LODs ", dimensionGroupName(dimensionGroup, log));
      for (auto lod=0; lod<volumeDataLayoutDescriptor.GetLODLevels(); ++lod) {
          auto status = volumeDataAccessManager.GetVDSProduceStatus(dimensionGroup, lod, channel);
          if (status!=OpenVDS::VDSProduceStatus::Unavailable) {
            fprintf(log, " %d:%c", lod, status==OpenVDS::VDSProduceStatus::Normal ? 'N' : 'R');
          }
          if (bestVDSProduceStatus==OpenVDS::VDSProduceStatus::Unavailable && status!=OpenVDS::VDSProduceStatus::Unavailable) {
            bestChannel = 0;
            bestDimensionGroup = dimensionGroup;
            bestLOD = lod;
            bestVDSProduceStatus = status;
          }
          if (bestVDSProduceStatus==OpenVDS::VDSProduceStatus::Remapped && status==OpenVDS::VDSProduceStatus::Normal) {
            bestChannel = 0;
            bestDimensionGroup = dimensionGroup;
            bestLOD = lod;
            bestVDSProduceStatus = status;
          }
          if (bestVDSProduceStatus==status && lod>bestLOD) {
            bestChannel = 0;
            bestDimensionGroup = dimensionGroup;
            bestLOD = lod;
            bestVDSProduceStatus = status;
          }
      }
      fprintf(log, "\n");
    }
  }
  fprintf(log, "\nEstimated best:\n");
  if (bestChannel<0 || bestLOD<0) {
    fprintf(log,   "No suitable Channel-DimensionGroup-LOD found to query.\n");
    exit(1);
  }
  fprintf(log, "  Channel %d  dimension group %s  lod %d  produce status %c\n", bestChannel, dimensionGroupName(bestDimensionGroup, log), bestLOD,
         bestVDSProduceStatus==OpenVDS::VDSProduceStatus::Normal ? 'N' : 'R');

  // calculate what data we should request - 4D 
  // 0 - sample - full
  // 1 - trace  - full for gather, single for il,xl
  // 2 - xl     - single for gather, full for inline
  // 3 - il     - single
  int voxelMin[OpenVDS::Dimensionality_Max] = { 0, 0, 0, 0, 0, 0};
  int voxelMax[OpenVDS::Dimensionality_Max] = { 1, 1, 1, 1, 1, 1};
  std::vector<float> buffer;

  // request a volume subset for dimensionalitys 2, 3, 4
  if (volumeDataLayout->GetDimensionality()==2 || volumeDataLayout->GetDimensionality()==3 || volumeDataLayout->GetDimensionality()==4) {
    uint32_t quarterFactor{1};
    do {
      // show a xline or gather - traces x samples
      voxelMax[0] = volumeDataLayout->GetDimensionNumSamples(0) / quarterFactor;  // divide both dimensions by 2 on a retry
      voxelMax[1] = volumeDataLayout->GetDimensionNumSamples(1) / quarterFactor;

      // pick middle of xline, inline
      voxelMin[2] = volumeDataLayout->GetDimensionNumSamples(2)/2;
      voxelMax[2] = voxelMin[2] + 1;
      voxelMin[3] = volumeDataLayout->GetDimensionNumSamples(3)/2;
      voxelMax[3] = voxelMin[3] + 1;

      // size buffer
      buffer.resize(voxelMax[0] * voxelMax[1]);
      auto request = volumeDataAccessManager.RequestVolumeSubset<float>(buffer.data(), buffer.size() * sizeof(float), bestDimensionGroup, 0, 0, voxelMin, voxelMax);

      // check completed
      if (!request->WaitForCompletion()) {
        fprintf(log, "*** Failed to wait for completion on horizon / xline / gather request. Try %u / 4.\n\n", quarterFactor);
      } else {
        fprintf(log, "Received a horizon / xline / gather.\n");
        break;
      }
    } while (quarterFactor++ < 5);

    // check if we hit the limit
    if (quarterFactor==5) {
      fprintf(log, "Stopping. Unable to retrieve data from VDS.\n");
      exit(5);
    }
  } else {
    fprintf(log, "Unable to thumbnail VDS dimensionality of %d.\nStopping.\n", volumeDataLayout->GetDimensionality());
    exit(6);
  }

  // calculate avg, std dev - silently ignores >fabs(10e10)
  float mean{0.f}, std_dev{0.f};
  compute_mean_std_dev(buffer.data(), buffer.size(), mean, std_dev);

  // scale traces and samples into square graphic
  int samples = voxelMax[0];
  int traces = voxelMax[1];
  int hpixels = (traces>=samples) ? sidePixels-20 : std::max(traces*sidePixels/samples, sidePixels/2);
  int vpixels = (samples>=traces) ? sidePixels-20 : std::max(samples*sidePixels/traces, sidePixels/2);

  // create graphic and draw into it
  uint32_t *graphic = new uint32_t[sidePixels*sidePixels];
  memset(graphic, 0xF0, sidePixels*sidePixels*sizeof(uint32_t));

  // select color bar
  uint32_t *transfer_function{nullptr};
  uint32_t gain{0};
  if (volumeDataLayout->GetDimensionality()==2 && !hasSampleAxis) {
    // render as horizon
    printf("horizon\n");
    transfer_function = topological_tf;
    gain = 1;     // try and use fullish range
  } else {
    printf("seismic\n");
    transfer_function = redwhiteblack_tf;
    gain = 3;     // push data towards the middle
  }

  // draw columns
  for (uint32_t x=0; x<sidePixels; ++x) {
    // skip trace? either side of narrow gather
    if (x<=(sidePixels-hpixels)/2 || sidePixels-x<=(sidePixels-hpixels)/2) {
      // should write background into graphic
      continue;
    }
    // calc trace position - position of start of trace
    int trace_pix = (x - (sidePixels-hpixels)/2);
    int trace_seq = trace_pix * traces / hpixels;
    int trace_pos = trace_seq * samples;

    // draw column / trace
    for (uint32_t y=0; y<sidePixels; ++y) {
      // add some top / bottom border
      if (y<=(sidePixels-vpixels)/2 || sidePixels-y<=(sidePixels-vpixels)/2) {
        // should write background into graphic
        continue;
      }

      // calc sample position
      int sample_pix = (y - (sidePixels-vpixels)/2);
      int sample_pos = sample_pix * sidePixels / vpixels;

      // get value at trace - sample
      float value = 128 + (buffer.data()[trace_pos + sample_pos] - mean) * 128 / std_dev / gain;

      // get value as 0 - 255 centered around mean +/- 2*std_dev
      uint8_t color_index = uint8_t(value < 0 ? 0 : value > 255 ? 255 : value);

      // copy color bar value into graphic
      graphic[x + y*sidePixels] = transfer_function[color_index];
    }
  }

  stbi_write_png(thumbnail_file.c_str(), sidePixels, sidePixels, 4, graphic, sidePixels*sizeof(uint32_t));
  fprintf(log, "*** Wrote thumbnail.\n\n");
  return 0;
}
