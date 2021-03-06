/*=========================================================================

  Program:   Lesion Segmentation CLIs for Slicer4
  Module:    $HeadURL$
  Language:  C++
  Date:      $Date$
  Version:   $Revision$

  Copyright (c) Biomedical Mining, LLC. All rights reserved.
  See https://github.com/msscully/LesionSegmentation for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#if defined(_MSC_VER)
#pragma warning ( disable : 4786 )
#endif

#include <iostream>
#include <vector>
#include <utility>
#include "itkPluginUtilities.h" 
#include "itkImage.h"
#include "itkImageFileReader.h"
#include "itkImageFileWriter.h"
#include "itkListSample.h"
#include "itkMembershipSample.h"
#include "itkVector.h"
#include "itkPluginFilterWatcher.h"
#include "itkImageToVectorImageFilter.h"
#include "itkMaskImageFilter.h"
#include "itkImageRegionIteratorWithIndex.h"
#include "itkInvertIntensityImageFilter.h"
#include "itkCastImageFilter.h"
#include "itkBinaryBallStructuringElement.h"
#include "itkGrayscaleDilateImageFilter.h"
#include "itkFlipImageFilter.h"
#include "itkSubtractImageFilter.h"
#include "itkScalarImageKmeansImageFilter.h"
#include "itkBinaryThresholdImageFilter.h"
#include "itkLabelStatisticsImageFilter.h"
#include "itkSignedMaurerDistanceMapImageFilter.h"
#include "itkGrayscaleErodeImageFilter.h"
#include "itkMedianImageFilter.h"
#include "itkHistogramMatchingImageFilter.h"
#include "itkStatisticsImageFilter.h"
#include "itkChangeInformationImageFilter.h"
#include "nanoflann.hpp"
#include "LesionSegmentationModel.h"
#include "TrainModelCLP.h"

/* Use an anonymous namespace to keep class types and function names
// from colliding when module is used as shared object module.  Every
// thing should be in an anonymous namespace except for the module
// entry point, e.g. main()
*/ 
namespace
{

typedef short      PixelType;
const unsigned int Dimension = 3;
typedef itk::Image< PixelType,  Dimension >  ImageType;
typedef LesionSegmentationModel::TrainingArrayType MeasurementArrayType;
typedef LesionSegmentationModel::FLANNMatrixType FLANNMatrixType;
typedef LesionSegmentationModel::LabelVectorType LabelVectorType;

const PixelType imageExclusion = itk::NumericTraits<PixelType>::min( PixelType() );

template<class DetectedPixelType> ImageType::Pointer castImage(std::string& imageFileName,DetectedPixelType)
{
  typedef itk::Image<DetectedPixelType,Dimension> DetectedImageType;
  typedef itk::ImageFileReader< DetectedImageType  >  ImageReaderType;
  typename ImageReaderType::Pointer imageReader = ImageReaderType::New();
  imageReader->SetFileName(imageFileName);

  typedef itk::CastImageFilter<DetectedImageType,ImageType> CastImageFilterType;
  typename CastImageFilterType::Pointer castImageFilter = CastImageFilterType::New();
  castImageFilter->SetInput(imageReader->GetOutput());
  castImageFilter->Update();
  typename ImageType::Pointer castImage = castImageFilter->GetOutput();

  return castImage;
}

ImageType::Pointer ReadAndConvertImage(std::string& imageFileName)
{
  itk::ImageIOBase::IOPixelType     pixelType;
  itk::ImageIOBase::IOComponentType componentType;

  itk::GetImageType(imageFileName, pixelType, componentType);

  try
    {
    switch (componentType)
      {
      case itk::ImageIOBase::UCHAR:
        return castImage<unsigned char>(imageFileName,static_cast<unsigned char>(0));
        break;
      case itk::ImageIOBase::CHAR:
        return castImage<char>(imageFileName,static_cast<char>(0));
        break;
      case itk::ImageIOBase::USHORT:
        return castImage<unsigned short>(imageFileName,static_cast<unsigned short>(0));
        break;
      case itk::ImageIOBase::SHORT:
        return castImage<short>(imageFileName,static_cast<short>(0));
        break;
      case itk::ImageIOBase::UINT:
        return castImage<unsigned int>(imageFileName,static_cast<unsigned int>(0));
        break;
      case itk::ImageIOBase::INT:
        return castImage<int>(imageFileName,static_cast<int>(0));
        break;
      case itk::ImageIOBase::ULONG:
        return castImage<unsigned long>(imageFileName,static_cast<unsigned long>(0));
        break;
      case itk::ImageIOBase::LONG:
        return castImage<long>(imageFileName,static_cast<long>(0));
        break;
      case itk::ImageIOBase::FLOAT:
        return castImage<float>(imageFileName,static_cast<float>(0));
        break;
      case itk::ImageIOBase::DOUBLE:
        return castImage<double>(imageFileName,static_cast<double>(0));
        break;
      case itk::ImageIOBase::UNKNOWNCOMPONENTTYPE:
      default:
        std::cout << "  ERROR: Unknown pixel type!" << std::endl;
        exit(EXIT_FAILURE);
      }
    }
  catch( itk::ExceptionObject &excep)
    {
    std::cerr << ": exception caught !" << std::endl;
    std::cerr << excep << std::endl;
    exit(EXIT_FAILURE);
    }
}

ImageType::Pointer HistogramMatch(ImageType::Pointer referenceImage, ImageType::Pointer movingImage)
{
  size_t numMatchPoints = 1000;

  typedef itk::HistogramMatchingImageFilter< ImageType, ImageType> HistogramMatchingFilterType;
  HistogramMatchingFilterType::Pointer histogramMatchingFilter = HistogramMatchingFilterType::New();

  typedef HistogramMatchingFilterType::OutputImageType HistMatchType;

  typedef itk::StatisticsImageFilter< ImageType > StatisticsFilterType;
  StatisticsFilterType::Pointer statisticsFilter = StatisticsFilterType::New();
  typedef itk::CastImageFilter< HistMatchType,ImageType > CastFilterType;
  CastFilterType::Pointer castFilter = CastFilterType::New();

  size_t numOfBins = 0;

  statisticsFilter->SetInput(referenceImage);
  statisticsFilter->Update();
  numOfBins = statisticsFilter->GetMaximum() - statisticsFilter->GetMinimum() + 1;

  histogramMatchingFilter->SetSourceImage(movingImage);
  histogramMatchingFilter->SetReferenceImage(referenceImage);
  histogramMatchingFilter->SetNumberOfHistogramLevels(numOfBins);
  histogramMatchingFilter->SetNumberOfMatchPoints(numMatchPoints);

  castFilter->SetInput(histogramMatchingFilter->GetOutput());
  castFilter->Update();
  return castFilter->GetOutput();
}

MeasurementArrayType CalculateSignedRangeInverse(MeasurementArrayType& mins, MeasurementArrayType& maxes)
{
  MeasurementArrayType signedRangeInverse; signedRangeInverse.Fill(0);

  for( unsigned int i = 0; i<mins.Size(); i++)
    {
    signedRangeInverse[i] = 2 / (maxes[i]-mins[i]);
    }

  return signedRangeInverse;
}

int DoIt(std::vector<std::string> inputT1Volumes, std::vector<std::string> inputT2Volumes, std::vector<std::string> inputFLAIRVolumes, std::vector<std::string> inputMaskVolumes, std::vector<std::string> inputLesionVolumes, std::string outputModel, unsigned int inputIndexOfBestImages, unsigned int inputPercentNonLesion)
{
  typedef itk::ImageFileReader< ImageType  >  ReaderType;

  typedef itk::MaskImageFilter< ImageType, ImageType, ImageType > MaskFilterType;
  MaskFilterType::Pointer flairMaskFilter = MaskFilterType::New();
  MaskFilterType::Pointer t1MaskFilter = MaskFilterType::New();
  MaskFilterType::Pointer t2MaskFilter = MaskFilterType::New();
  MaskFilterType::Pointer clippedT1Filter = MaskFilterType::New();
  MaskFilterType::Pointer refT1MaskFilter = MaskFilterType::New();
  MaskFilterType::Pointer refT2MaskFilter = MaskFilterType::New();
  MaskFilterType::Pointer refFLAIRMaskFilter = MaskFilterType::New();

  LesionSegmentationModel lesionSegmentationModel = LesionSegmentationModel();
  const unsigned char numFeatures = lesionSegmentationModel.m_NumFeatures;

  MeasurementArrayType trainMeans; trainMeans.Fill(0);
  MeasurementArrayType trainSigmas; trainSigmas.Fill(0);
  MeasurementArrayType trainMins; trainMins.Fill(0);
  MeasurementArrayType trainMaxes; trainMaxes.Fill(0);

  //TODO: reserve an estimated size based on number of input images,
  // the resolution of those images, and the %nonlesion samples.
  LabelVectorType sampleLabels;

  FLANNMatrixType trainingSamples;

  typedef itk::ImageRegionIteratorWithIndex< ImageType > ImageRegionIteratorType; 

  srand((unsigned)time(0)); 

  typedef itk::BinaryThresholdImageFilter<ImageType,ImageType> BinaryThresholdFilterType;
  BinaryThresholdFilterType::Pointer maskBinaryFilter = BinaryThresholdFilterType::New();

  // Indexes are -1 because the input is 1 indexed, not zero indexed.
  ImageType::Pointer refT1 = ReadAndConvertImage(inputT1Volumes[inputIndexOfBestImages-1]);
  ImageType::Pointer refT2 = ReadAndConvertImage(inputT2Volumes[inputIndexOfBestImages-1]);
  ImageType::Pointer refFLAIR = ReadAndConvertImage(inputFLAIRVolumes[inputIndexOfBestImages-1]);
  ImageType::Pointer refMask = ReadAndConvertImage(inputMaskVolumes[inputIndexOfBestImages-1]);

  maskBinaryFilter->SetInput( refMask );
  maskBinaryFilter->SetLowerThreshold( 1 );
  maskBinaryFilter->SetOutsideValue( 0 );
  maskBinaryFilter->SetInsideValue( 1 );

  refT1MaskFilter->SetInput1(refT1);
  refT1MaskFilter->SetInput2(maskBinaryFilter->GetOutput());
  refT1MaskFilter->Update();

  refT2MaskFilter->SetInput1(refT2);
  refT2MaskFilter->SetInput2(maskBinaryFilter->GetOutput());
  refT2MaskFilter->Update();

  refFLAIRMaskFilter->SetInput1(refFLAIR);
  refFLAIRMaskFilter->SetInput2(maskBinaryFilter->GetOutput());
  refFLAIRMaskFilter->Update();

  typedef itk::BinaryBallStructuringElement<PixelType,Dimension > StructuringElementType;
  StructuringElementType structuringElement2;  
  structuringElement2.SetRadius( 2 ); 
  structuringElement2.CreateStructuringElement();  

  StructuringElementType structuringElement3;  
  structuringElement3.SetRadius( 3 ); 
  structuringElement3.CreateStructuringElement();  
 
  typedef itk::GrayscaleDilateImageFilter< ImageType, ImageType, StructuringElementType > DilateFilterType; 
  DilateFilterType::Pointer flairGrayscaleDilate2 = DilateFilterType::New();
  flairGrayscaleDilate2->SetKernel( structuringElement2 );

  typedef itk::FlipImageFilter <ImageType> FlipImageFilterType;
  FlipImageFilterType::Pointer flipT1Filter = FlipImageFilterType::New();
  flipT1Filter->FlipAboutOriginOff();
  FlipImageFilterType::FlipAxesArrayType flipAxes;
  flipAxes[0] = true;
  flipAxes[1] = false;
  flipAxes[2] = false;
  flipT1Filter->SetFlipAxes(flipAxes);

  typedef itk::SubtractImageFilter <ImageType,ImageType> SubtractFilterType;
  SubtractFilterType::Pointer subtractFilter = SubtractFilterType::New();

  typedef itk::ChangeInformationImageFilter < ImageType > ChangeImageFilterType;
  ChangeImageFilterType::Pointer changeImageFilter = ChangeImageFilterType::New();

  typedef itk::LabelStatisticsImageFilter< ImageType, ImageType > LabelStatisticsFilterType;
  typedef LabelStatisticsFilterType::RealType StatisticRealType;
  LabelStatisticsFilterType::Pointer statisticsFilter = LabelStatisticsFilterType::New();

  typedef itk::ScalarImageKmeansImageFilter< ImageType > KMeansFilterType;
  typedef KMeansFilterType::RealPixelType RealPixelType;
  KMeansFilterType::Pointer kmeansFilter = KMeansFilterType::New();
  typedef KMeansFilterType::OutputImageType LabelImageType;

  typedef itk::CastImageFilter<LabelImageType,ImageType> CastFilterType;
  CastFilterType::Pointer castFilter = CastFilterType::New();

  typedef itk::BinaryThresholdImageFilter<LabelImageType,LabelImageType> BinaryThresholdLabelFilterType;
  BinaryThresholdLabelFilterType::Pointer grayBinaryFilter = BinaryThresholdLabelFilterType::New();
  BinaryThresholdLabelFilterType::Pointer whiteBinaryFilter = BinaryThresholdLabelFilterType::New();
  BinaryThresholdLabelFilterType::Pointer csfBinaryFilter = BinaryThresholdLabelFilterType::New();

  typedef itk::SignedMaurerDistanceMapImageFilter< LabelImageType, LabelImageType> DistanceMapFilterType;
  DistanceMapFilterType::Pointer grayDistanceMapFilter = DistanceMapFilterType::New();
  DistanceMapFilterType::Pointer whiteDistanceMapFilter = DistanceMapFilterType::New();
  typedef DistanceMapFilterType::OutputImageType DistanceMapImageType;

  typedef itk::GrayscaleErodeImageFilter< ImageType, ImageType, StructuringElementType > ErodeFilterType; 
  ErodeFilterType::Pointer flairGrayscaleErode3 = ErodeFilterType::New();
  flairGrayscaleErode3->SetKernel( structuringElement3 );

  typedef itk::MedianImageFilter< ImageType, ImageType > MedianFilterType;
  MedianFilterType::Pointer flairMedian3Filter = MedianFilterType::New();

  MedianFilterType::InputSizeType radius3;
  radius3[0] = 3;
  radius3[1] = 3;
  radius3[2] = 3;

  size_t lesionCount =0;

  try
    {

    for (unsigned int i=0; i<inputFLAIRVolumes.size(); i++)
      {
      /* Load the ith training images */
      ImageType::Pointer t1Image = ReadAndConvertImage(inputT1Volumes[i]);
      ImageType::Pointer t2Image = ReadAndConvertImage(inputT2Volumes[i]);
      ImageType::Pointer flairImage = ReadAndConvertImage(inputFLAIRVolumes[i]);
      ImageType::Pointer tempMaskImage = ReadAndConvertImage(inputMaskVolumes[i]);
      ImageType::Pointer lesionImage = ReadAndConvertImage(inputLesionVolumes[i]);

      maskBinaryFilter->SetInput( tempMaskImage );
      maskBinaryFilter->SetLowerThreshold( 1 );
      maskBinaryFilter->SetOutsideValue( 0 );
      maskBinaryFilter->SetInsideValue( 1 );
      maskBinaryFilter->Modified();
      ImageType::Pointer maskImage = maskBinaryFilter->GetOutput();
      
      t1MaskFilter->SetInput1( t1Image );
      t1MaskFilter->SetInput2( maskImage );
      t1MaskFilter->SetOutsideValue(0);
      t1MaskFilter->Modified();
      t1MaskFilter->Update();

      t2MaskFilter->SetInput1( t2Image );
      t2MaskFilter->SetInput2( maskImage );
      t2MaskFilter->SetOutsideValue(0);
      t2MaskFilter->Modified();
      t2MaskFilter->Update();

      flairMaskFilter->SetInput1( flairImage );
      flairMaskFilter->SetInput2( maskImage );
      flairMaskFilter->SetOutsideValue(0);
      flairMaskFilter->Modified();
      flairMaskFilter->Update();

      ImageType::Pointer t1ImageHistMatched = HistogramMatch(refT1MaskFilter->GetOutput(),t1MaskFilter->GetOutput());
      ImageType::Pointer t2ImageHistMatched = HistogramMatch(refT2MaskFilter->GetOutput(),t2MaskFilter->GetOutput());
      ImageType::Pointer flairImageHistMatched = HistogramMatch(refFLAIRMaskFilter->GetOutput(),flairMaskFilter->GetOutput());

      /* Iterate over the flair, get it's intensity and it's location.  Use those
       ** values to calculate the mean and standard deviation.  Iterate over the 
       ** flair again, zero-mean and sigma correct, and push the measurement vector
       ** onto a listsample.
       */

      /* Top numFeatures (10) most informative features
         Dilate FLAIR radius=2
         T1w flipped difference
         Normalized Z location
         Normalized X location
         Distance to white matter
         Normalized Y location
         Distance to gray matter
         Erode Flair radius=3
         Normalized T2w intensity
         Median FLAIR radius=3
        */


      flairGrayscaleDilate2->SetInput( flairImageHistMatched );
      flairGrayscaleDilate2->Modified();
      flairGrayscaleDilate2->Update();

      flipT1Filter->SetInput( t1ImageHistMatched );
      flipT1Filter->Modified();
      flipT1Filter->Update();

      changeImageFilter->SetInput( flipT1Filter->GetOutput());
      changeImageFilter->SetOutputOrigin( t1ImageHistMatched->GetOrigin() );
      changeImageFilter->SetOutputDirection( t1ImageHistMatched->GetDirection() );
      changeImageFilter->SetChangeOrigin( true );
      changeImageFilter->SetChangeDirection( true );
      changeImageFilter->Modified();
      changeImageFilter->Update();

      subtractFilter->SetInput1( t1ImageHistMatched );
      subtractFilter->SetInput2( changeImageFilter->GetOutput() );
      subtractFilter->Modified();
      subtractFilter->Update();

      clippedT1Filter->SetInput1( t1ImageHistMatched );
      clippedT1Filter->SetInput2( maskImage );
      clippedT1Filter->SetOutsideValue( imageExclusion );
      clippedT1Filter->Modified();
      clippedT1Filter->Update();

      statisticsFilter->SetInput( t1ImageHistMatched );
      statisticsFilter->SetLabelInput( maskImage );
      statisticsFilter->Modified();
      statisticsFilter->Update();

      const StatisticRealType imageMean = statisticsFilter->GetMean( 1 );
      const StatisticRealType imageSigma = statisticsFilter->GetSigma( 1 );

      const RealPixelType csfInitialMean = imageMean - 2*imageSigma;
      const RealPixelType whiteInitialMean = imageMean + imageSigma;
      const RealPixelType grayInitialMean = imageMean - imageSigma/5;

      RealPixelType backgroundInitialMean = imageExclusion;
      kmeansFilter->AddClassWithInitialMean( backgroundInitialMean );
      kmeansFilter->AddClassWithInitialMean( csfInitialMean );
      kmeansFilter->AddClassWithInitialMean( grayInitialMean );
      kmeansFilter->AddClassWithInitialMean( whiteInitialMean );
      kmeansFilter->SetInput( clippedT1Filter->GetOutput() );
      kmeansFilter->Modified();
      castFilter->SetInput(kmeansFilter->GetOutput());
      castFilter->Modified();
      castFilter->Update();

      grayBinaryFilter->SetInput(kmeansFilter->GetOutput());
      grayBinaryFilter->SetLowerThreshold(2);
      grayBinaryFilter->SetUpperThreshold(2);
      grayBinaryFilter->Modified();
      grayDistanceMapFilter->SetInput( grayBinaryFilter->GetOutput() );
      grayDistanceMapFilter->UseImageSpacingOff();
      grayDistanceMapFilter->SquaredDistanceOff();
      grayDistanceMapFilter->Modified();
      grayDistanceMapFilter->Update();

      whiteBinaryFilter->SetInput(kmeansFilter->GetOutput());
      whiteBinaryFilter->SetLowerThreshold(1);
      whiteBinaryFilter->SetUpperThreshold(1);
      whiteBinaryFilter->Modified();
      whiteDistanceMapFilter->SetInput( whiteBinaryFilter->GetOutput() );
      whiteDistanceMapFilter->UseImageSpacingOff();
      whiteDistanceMapFilter->SquaredDistanceOff();
      whiteDistanceMapFilter->Modified();
      whiteDistanceMapFilter->Update();

      flairGrayscaleErode3->SetInput( flairImageHistMatched );
      flairGrayscaleErode3->Modified();
      flairGrayscaleErode3->Update();

      flairMedian3Filter->SetInput( flairImageHistMatched );
      flairMedian3Filter->SetRadius( radius3 );
      flairMedian3Filter->Modified();
      flairMedian3Filter->Update();
 
      ImageRegionIteratorType flairItr( flairImageHistMatched,flairImageHistMatched->GetRequestedRegion() ); 

      size_t count = 0;

      /* First pass is to calculate the mean and standard deviation of the x, y, z
       ** voxel locations and the mean and std of the flair intensity.
       */
      MeasurementArrayType tempSamples; tempSamples.Fill(0);

      for ( flairItr.GoToBegin(); !flairItr.IsAtEnd(); ++flairItr) 
        { 
        ImageType::IndexType idx = flairItr.GetIndex();

        if(maskImage->GetPixel(idx) != 0)
          {
          tempSamples[0] = idx[0];
          tempSamples[1] = idx[1];
          tempSamples[2] = idx[2];
          tempSamples[3] = flairGrayscaleDilate2->GetOutput()->GetPixel(idx);
          tempSamples[4] = subtractFilter->GetOutput()->GetPixel(idx);
          tempSamples[5] = grayDistanceMapFilter->GetOutput()->GetPixel(idx);
          tempSamples[6] = whiteDistanceMapFilter->GetOutput()->GetPixel(idx);
          tempSamples[7] = flairGrayscaleErode3->GetOutput()->GetPixel(idx);
          tempSamples[8] = t2ImageHistMatched->GetPixel(idx);
          tempSamples[9] = flairMedian3Filter->GetOutput()->GetPixel(idx);

          for( unsigned int i=0; i<tempSamples.Size(); i++ )
            {
            trainMeans[i] += tempSamples[i];
            trainSigmas[i] += tempSamples[i] * tempSamples[i];

            if( tempSamples[i] < trainMins[i] )
              {
              trainMins[i] = tempSamples[i];
              }
            if(tempSamples[i] > trainMaxes[i])
              {
              trainMaxes[i] = tempSamples[i];
              }
            }

          ++count;
          }
        }

      for( unsigned int w=0; w<trainMeans.Size(); w++ )
        {
        trainMeans[w] = trainMeans[w] / count;
        trainSigmas[w] = trainSigmas[w] / count;
        trainSigmas[w] = sqrt(trainSigmas[w] - (trainMeans[w]*trainMeans[w]));
        }

      /* Grab the index and value, zero-mean and sigma correct, create a measurement vector,
       ** and push it onto the list of samples while pushing the label onto the list of labels.
       */
      for ( flairItr.GoToBegin(); !flairItr.IsAtEnd(); ++flairItr)
        {

        ImageType::IndexType idx = flairItr.GetIndex();
        if(maskImage->GetPixel(idx) != 0)
          {

          MeasurementArrayType tempMeasurement; tempMeasurement.Fill(0);

          tempMeasurement.SetElement( 0, (idx[0]-trainMeans[0])/trainSigmas[0] ); 
          tempMeasurement.SetElement( 1, (idx[1]-trainMeans[1])/trainSigmas[1] ); 
          tempMeasurement.SetElement( 2, (idx[2]-trainMeans[2])/trainSigmas[2] ); 
          tempMeasurement.SetElement( 3, (flairGrayscaleDilate2->GetOutput()->GetPixel(idx)-trainMeans[3]-1)/trainSigmas[3] ); 
          tempMeasurement.SetElement( 4, (subtractFilter->GetOutput()->GetPixel(idx)-trainMeans[4]-1)/trainSigmas[4] ); 
          tempMeasurement.SetElement( 5, (grayDistanceMapFilter->GetOutput()->GetPixel(idx)-trainMeans[5]-1)/trainSigmas[5] ); 
          tempMeasurement.SetElement( 6, (whiteDistanceMapFilter->GetOutput()->GetPixel(idx)-trainMeans[6]-1)/trainSigmas[6] ); 
          tempMeasurement.SetElement( 7, (flairGrayscaleErode3->GetOutput()->GetPixel(idx)-trainMeans[7]-1)/trainSigmas[7] ); 
          tempMeasurement.SetElement( 8, (t2ImageHistMatched->GetPixel(idx) -trainMeans[8]-1)/trainSigmas[8] ); 
          tempMeasurement.SetElement( 9, (flairMedian3Filter->GetOutput()->GetPixel(idx)-trainMeans[9]-1)/trainSigmas[9] ); 


          if(lesionImage->GetPixel(idx) !=0)          
            {
            trainingSamples.push_back(tempMeasurement);
            sampleLabels.push_back(1);
            lesionCount++;
            }
          else if ((unsigned int)(rand()%100+1) <= inputPercentNonLesion) /* We only want a fraction of non-lesion voxels */
            {
            trainingSamples.push_back(tempMeasurement);
            sampleLabels.push_back(0);
            }
          } 
        }
      }

    MeasurementArrayType signedRangeInverse = CalculateSignedRangeInverse(trainMins,trainMaxes);

    const size_t numSamples = trainingSamples.size();
    
    std::cout <<"numSamples: " << numSamples << std::endl;
    std::cout <<"numLesions: " << lesionCount << std::endl;

    for( unsigned int i=0; i<numSamples; i++)
      {
      for( unsigned int j=0; j<numFeatures; j++)
        {
        trainingSamples[i][j] = signedRangeInverse[j] * (trainingSamples[i][j] - trainMins[j]) - 1;
        }
      }

    lesionSegmentationModel.SetTrainingMins(trainMins);
    lesionSegmentationModel.SetTrainingMaxes(trainMaxes);
    lesionSegmentationModel.SetTrainingSignedRangeInverse(signedRangeInverse);
    lesionSegmentationModel.SetLabelsSize(sampleLabels.size());
    lesionSegmentationModel.SetTrainingLabels(sampleLabels);
    lesionSegmentationModel.SetFLANNDataset(trainingSamples);
    lesionSegmentationModel.SaveModel(outputModel);

    std::cout << "Models saved." << std::endl;

    }
  catch (itk::ExceptionObject &excep)
    {
    std::cerr << "Exception caught !" << std::endl;
    std::cerr << excep << std::endl;
    return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
} // End anonymous namespace

int main( int argc, char * argv[] )
{
  PARSE_ARGS;

  bool violated=false;
  if (inputFLAIRVolumes.size() == 0) { violated = true; std::cout << "  --inputFLAIRVolumes Required! "  << std::endl; }
  if (inputLesionVolumes.size() == 0) { violated = true; std::cout << "  --inputLesionVolumes Required! "  << std::endl; }
  if (inputMaskVolumes.size() == 0) { violated = true; std::cout << "  --inputMaskVolumes Required! "  << std::endl; }
  if (inputT1Volumes.size() == 0) { violated = true; std::cout << "  --inputT1Volumes Required! "  << std::endl; }
  if (inputT2Volumes.size() == 0) { violated = true; std::cout << "  --inputT2Volumes Required! "  << std::endl; }
  if (outputModel.size() == 0) { violated = true; std::cout << "  --outputModel Required! "  << std::endl; }
  if ((inputFLAIRVolumes.size() != inputLesionVolumes.size()) && 
    (inputMaskVolumes.size() != inputFLAIRVolumes.size()) &&
    (inputT1Volumes.size() != inputFLAIRVolumes.size()) &&
    (inputT2Volumes.size() != inputFLAIRVolumes.size())) 
    { violated = true; std::cout << "  the number of files after --inputT1Volumes, --inputT2Volumes, --inputFLAIRVolumes, --inputLesionVolumes, and --inputMaskVolumes must all be equal! "  << std::endl;
    }
  if (violated) exit(EXIT_FAILURE);

  try
    {
    return DoIt(inputT1Volumes,inputT2Volumes,inputFLAIRVolumes,inputMaskVolumes,inputLesionVolumes,outputModel,inputIndexOfBestImages,inputPercentNonLesion);
    }
  catch( itk::ExceptionObject & excep )
    {
    std::cerr << argv[0] << ": exception caught !" << std::endl;
    std::cerr << excep << std::endl;
    return EXIT_FAILURE;
    }
}
