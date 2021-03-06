/*
 * SimHDF5Detector.cpp
 *
 *  Created on: 13 Apr 2015
 *      Author: gnx91527
 */

#include <epicsThread.h>
#include <iocsh.h>
#include <drvSup.h>
#include <epicsExport.h>
#include <iostream>
#include <sstream>
#include "SimHDF5Detector.h"

static const char *driverName = "SimHDF5Detector";


/** C function called by newly created thread.
  * \param[in] drvPvt pointer to a void that is supplied by the thread create function.
  *
  * C function called in a seperate thread.  The pointer will point to the SimHDF5Detector
  * object that created the thread and can be used to call the appropriate method.
  */
static void SimHDF5DetectorTaskC(void *drvPvt)
{
  // Cast our void pointer into the SimHDF5Detector class
  SimHDF5Detector *pPvt = (SimHDF5Detector *)drvPvt;
  // Call the acquisition task method for the SimHDF5Detector
  pPvt->acqTask();
}

/** Constructor.
  * \param[in] portName name of the asyn port for this driver.
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is
  *            allowed to allocate. Set this to 0 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is
  *            allowed to allocate. Set this to 0 to allow an unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  *
  * Construct a new SimHDF5Detector object.  Acquisition thread is started during the
  * construction of the object.
  */
SimHDF5Detector::SimHDF5Detector(const char *portName,
                                 int maxBuffers,
                                 size_t maxMemory,
                                 int priority,
                                 int stackSize)
  : ADDriver(portName,
             1,
             NUM_ADSIM_DETECTOR_PARAMS,
             maxBuffers,
             maxMemory,
             0,
             0, // No interfaces beyond those set in ADDriver.cpp
             0, // ASYN_CANBLOCK=0, ASYN_MULTIDEVICE=0, autoConnect=1
             1, // autoConnect = 1
             priority,
             stackSize),
  validFile(false),
  pRaw(NULL)
{
  int status = asynSuccess;
  const char *functionName = "SimHDF5Detector";

  // Create parameters for control of the driver
  createParam(str_ADSim_Filename,      asynParamOctet,   &ADSim_Filename);
  createParam(str_ADSim_FileValid,     asynParamInt32,   &ADSim_FileValid);
  createParam(str_ADSim_NumOfDsets,    asynParamInt32,   &ADSim_NumOfDsets);
  createParam(str_ADSim_DsetIndex,     asynParamInt32,   &ADSim_DsetIndex);
  createParam(str_ADSim_DsetName,      asynParamOctet,   &ADSim_DsetName);
  createParam(str_ADSim_DsetNumDims,   asynParamInt32,   &ADSim_DsetNumDims);
  createParam(str_ADSim_DsetDim1,      asynParamInt32,   &ADSim_DsetDim1);
  createParam(str_ADSim_DsetDim2,      asynParamInt32,   &ADSim_DsetDim2);
  createParam(str_ADSim_DsetDim3,      asynParamInt32,   &ADSim_DsetDim3);
  createParam(str_ADSim_DsetDim4,      asynParamInt32,   &ADSim_DsetDim4);
  createParam(str_ADSim_DsetDim5,      asynParamInt32,   &ADSim_DsetDim5);
  createParam(str_ADSim_DsetDim6,      asynParamInt32,   &ADSim_DsetDim6);
  createParam(str_ADSim_XDim,          asynParamInt32,   &ADSim_XDim);
  createParam(str_ADSim_YDim,          asynParamInt32,   &ADSim_YDim);
  createParam(str_ADSim_DsetPath,      asynParamOctet,   &ADSim_DsetPath);

  // Create sensible default values for the parameters
  setStringParam (ADSim_Filename,    "");
  setIntegerParam(ADSim_FileValid,   0);
  setIntegerParam(ADSim_NumOfDsets,  0);
  setIntegerParam(ADSim_DsetIndex,   0);
  setStringParam (ADSim_DsetName,    "");
  setIntegerParam(ADSim_DsetNumDims, 0);
  setIntegerParam(ADSim_DsetDim1,    -1);
  setIntegerParam(ADSim_DsetDim2,    -1);
  setIntegerParam(ADSim_DsetDim3,    -1);
  setIntegerParam(ADSim_DsetDim4,    -1);
  setIntegerParam(ADSim_DsetDim5,    -1);
  setIntegerParam(ADSim_DsetDim6,    -1);
  setIntegerParam(ADSim_XDim,        0);
  setIntegerParam(ADSim_YDim,        0);
  setStringParam (ADSim_DsetPath,    "");

  // Set standard parameter values
  setStringParam (ADManufacturer, "Simulated detector");
  setStringParam (ADModel, "HDF5 reader");

  // Create the file reader object for parsing HDF5 simulated source files
  fileReader = std::tr1::shared_ptr<SimHDF5FileReader>(new SimHDF5FileReader());
  //fileReader = std::tr1::shared_ptr<SimHDF5MemoryReader>(new SimHDF5MemoryReader());

  // Create the epicsEvents for signalling to the acq task when acquisition starts and stops
  this->startEventId = epicsEventCreate(epicsEventEmpty);
  if (!this->startEventId){
      printf("%s:%s epicsEventCreate failure for start event\n", driverName, functionName);
      return;
  }
  this->stopEventId = epicsEventCreate(epicsEventEmpty);
  if (!this->stopEventId){
      printf("%s:%s epicsEventCreate failure for stop event\n", driverName, functionName);
      return;
  }

  // Create the thread that serves the images
  status = (epicsThreadCreate("SimHDF5DetectorTask",
                              epicsThreadPriorityMedium,
                              epicsThreadGetStackSize(epicsThreadStackMedium),
                              (EPICSTHREADFUNC)SimHDF5DetectorTaskC,
                              this) == NULL);
  if (status) {
      printf("%s:%s epicsThreadCreate failure for image task\n", driverName, functionName);
      return;
  }

}

/** Main acquisition task.
 *
 */
void SimHDF5Detector::acqTask()
{
  int status = asynSuccess;
  int imageCounter;
  int numImages, numImagesCounter;
  int arrayCounter;
  int imageMode;
  int arrayCallbacks;
  int acquire=0;
  NDArray *pImage;
  double acquireTime, acquirePeriod, delay;
  epicsTimeStamp startTime, endTime;
  double elapsedTime;
  const char *functionName = "simTask";

  this->lock();
  // Loop forever
  while (1){
    // If we are not acquiring then wait for a semaphore that is given when acquisition is started
    if (!acquire){
      setStringParam(ADStatusMessage, "Waiting to start acquisition");
      callParamCallbacks();
      // Close any previously opened dataset
      fileReader->cleanupDataset();
      // Release the lock while we wait for an event that says acquire has started, then lock again
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                "%s:%s: waiting for acquire to start\n", driverName, functionName);
      this->unlock();
      status = epicsEventWait(this->startEventId);
      this->lock();
      acquire = 1;
      setStringParam(ADStatusMessage, "Acquiring data");
      setIntegerParam(ADNumImagesCounter, 0);
      // Read the dataset index and prepare for reading
      int dsetIndex = 0;
      getIntegerParam(ADSim_DsetIndex, &dsetIndex);
      dsetIndex--;
      fileReader->prepareToReadDataset(fileReader->getDatasetKeys()[dsetIndex]);
    }

    // We are acquiring.
    // Get the current time
    epicsTimeGetCurrent(&startTime);
    getIntegerParam(ADImageMode, &imageMode);
    getIntegerParam(NDArrayCounter, &arrayCounter);

    // Get the exposure parameters
    getDoubleParam(ADAcquireTime, &acquireTime);
    getDoubleParam(ADAcquirePeriod, &acquirePeriod);

    setIntegerParam(ADStatus, ADStatusAcquire);

    // Call the callbacks to update any changes
    callParamCallbacks();

    this->unlock();
    status = epicsEventTryWait(this->stopEventId);
    this->lock();
    if (status == epicsEventWaitOK){
      acquire = 0;
      if (imageMode == ADImageContinuous){
        setIntegerParam(ADStatus, ADStatusIdle);
      } else {
        setIntegerParam(ADStatus, ADStatusAborted);
      }
      callParamCallbacks();
    }

    // Update the image
    this->unlock();
    status = readImage(arrayCounter);
    this->lock();
    if (status) continue;

    if (!acquire) continue;

    setIntegerParam(ADStatus, ADStatusReadout);
    // Call the callbacks to update any changes
    callParamCallbacks();

    pImage = this->pRaw;

    // Get the current parameters
    getIntegerParam(NDArrayCounter, &imageCounter);
    getIntegerParam(ADNumImages, &numImages);
    getIntegerParam(ADNumImagesCounter, &numImagesCounter);
    getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
    imageCounter++;
    numImagesCounter++;
    setIntegerParam(NDArrayCounter, imageCounter);
    setIntegerParam(ADNumImagesCounter, numImagesCounter);

    // Put the frame number and time stamp into the buffer
    pImage->uniqueId = imageCounter;
    pImage->timeStamp = startTime.secPastEpoch + startTime.nsec / 1.e9;

    // Get any attributes that have been defined for this driver
    this->getAttributes(pImage->pAttributeList);

    if (arrayCallbacks){
      // Call the NDArray callback
      // Must release the lock here, or we can get into a deadlock, because we can
      // block on the plugin lock, and the plugin can be calling us
      this->unlock();
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                "%s:%s: calling imageData callback\n", driverName, functionName);
      doCallbacksGenericPointer(pImage, NDArrayData, 0);
      this->lock();
    }

    // See if acquisition is done
    if ((imageMode == ADImageSingle) ||
        ((imageMode == ADImageMultiple) &&
        (numImagesCounter >= numImages))){

      // First do callback on ADStatus.
      setIntegerParam(ADStatus, ADStatusIdle);
      callParamCallbacks();

      acquire = 0;
      setIntegerParam(ADAcquire, acquire);
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                "%s:%s: acquisition completed\n", driverName, functionName);
    }

    // Call the callbacks to update any changes
    callParamCallbacks();

    // If we are acquiring then sleep for the acquire period minus elapsed time.
    if (acquire){
      epicsTimeGetCurrent(&endTime);
      elapsedTime = epicsTimeDiffInSeconds(&endTime, &startTime);
      delay = acquirePeriod - elapsedTime;
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                "%s:%s: delay=%f\n",
                driverName, functionName, delay);
      if (delay >= 0.0){
        // We set the status to waiting to indicate we are in the period delay
        setIntegerParam(ADStatus, ADStatusWaiting);
        callParamCallbacks();
        this->unlock();
        status = epicsEventWaitWithTimeout(this->stopEventId, delay);
        this->lock();
        if (status == epicsEventWaitOK){
          acquire = 0;
          if (imageMode == ADImageContinuous) {
            setIntegerParam(ADStatus, ADStatusIdle);
          } else {
            setIntegerParam(ADStatus, ADStatusAborted);
          }
          callParamCallbacks();
        }
      }
    }
  }
}

/** Sets an int32 parameter.
  * \param[in] pasynUser asynUser structure that contains the function code in pasynUser->reason.
  * \param[in] value The value for this parameter
  *
  * Takes action if the function code requires it.  This following parameters are supported:
  * ADAcquire - Start and stop an acquisition.
  * ADSim_DsetIndex - Select the dataset required for processing.
  * ADSim_XDim - Select which dataset dimension should be used for the width.
  * ADSim_YDim - Select which dataset dimension should be used for the height.
  */
asynStatus SimHDF5Detector::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
  int addr=0;
  int oldvalue = 0;
  int acquiring = 0;
  int function = pasynUser->reason;
  asynStatus status = asynSuccess;
  static const char *functionName = "writeInt32";

  getIntegerParam(ADAcquire, &acquiring);

  status = getAddress(pasynUser, &addr);
  if (status == asynSuccess){
    getIntegerParam(function, &oldvalue);

    // By default we set the value in the parameter library. If problems occur we set the old value back.
    setIntegerParam(function, value);

    asynPrint(pasynUser, ASYN_TRACE_FLOW,
              "%s:%s: function=%d, value=%d old=%d\n",
              driverName, functionName, function, value, oldvalue);

    if (function == ADAcquire){
      if (value && !acquiring){
        if (validFile){
          // Send an event to wake up the simulation task.
          // It won't actually start generating new images until we release the lock below
          epicsEventSignal(this->startEventId);
        } else {
          // Cannot start an acquisition without a valid file
          asynPrint(pasynUser, ASYN_TRACE_ERROR,
                    "%s:%s: Check the HDF5 file loaded is valid\n",
                    driverName, functionName);
          status = asynError;
          setIntegerParam(function, oldvalue);
        }
      }
      if (!value && acquiring){
        // This was a command to stop acquisition
        // Send the stop event
        epicsEventSignal(this->stopEventId);
      }
    } else if (function == ADSim_DsetIndex){
      // Call the updateSourceImage function
      status = readDatasetInfo();
      if (status == asynError){
        // If a bad value is set then revert it to the original
        setIntegerParam(function, oldvalue);
      }
    } else if (function == ADSim_XDim){
      // Call the updateSourceImage function
      status = updateSourceImage();
      if (status == asynError){
        // If a bad value is set then revert it to the original
        setIntegerParam(function, oldvalue);
      }
    } else if (function == ADSim_YDim){
      // Call the updateSourceImage function
      status = updateSourceImage();
      if (status == asynError){
        // If a bad value is set then revert it to the original
        setIntegerParam(function, oldvalue);
      }
    } else if (function == ADMinX || function == ADMinY || function == ADSizeX || function == ADSizeY){
      // Get the values and verify they are OK
      status = verifySizes();
      if (status == asynError){
        // If a bad value is set then revert it to the original
        setIntegerParam(function, oldvalue);
      } else {
        setArraySizes();
      }
    } else {
      if (function < FIRST_ADSIM_DETECTOR_PARAM){
        // If this parameter belongs to a base class call its method
        asynPrint(pasynUser, ASYN_TRACE_FLOW,
                  "%s:%s: calling base class function=%d, value=%d old=%d\n",
                  driverName, functionName, function, value, oldvalue);
        status = ADDriver::writeInt32(pasynUser, value);
      }
    }
    // Do callbacks so higher layers see any changes
    callParamCallbacks(addr, addr);
  }

  if (status){
    asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:%s: ERROR status=%d, function=%d, value=%d old=%d\n",
               driverName, functionName, status, function, value, oldvalue);
  } else {
    asynPrint(pasynUser, ASYN_TRACE_FLOW,
              "%s:%s: status=%d function=%d, value=%d old=%d\n",
              driverName, functionName, status, function, value, oldvalue);
  }
  return status;
}

/** Called when asyn clients call pasynOctet->write().
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Address of the string to write.
  * \param[in] nChars Number of characters to write.
  * \param[out] nActual Number of characters actually written.
  *
  * For all parameters it sets the value in the parameter library and calls any registered
  * callbacks.  The following parameters are supported:
  * ADSim_Filename - Load the HDF5 data file ready for an acquisition.
  */
asynStatus SimHDF5Detector::writeOctet(asynUser *pasynUser, const char *value, size_t nChars, size_t *nActual)
{
  int addr=0;
  int function = pasynUser->reason;
  asynStatus status = asynSuccess;
  char *fileName = new char[MAX_FILENAME_LEN];
  fileName[MAX_FILENAME_LEN - 1] = '\0';
  const char *functionName = "writeOctet";

  status = getAddress(pasynUser, &addr); if (status != asynSuccess) return(status);
  // Set the parameter in the parameter library.
  status = (asynStatus)setStringParam(addr, function, (char *)value);
  if (status != asynSuccess) return(status);

  if (function == ADSim_Filename){

    // Read the filename parameter
    getStringParam(ADSim_Filename, MAX_FILENAME_LEN-1, fileName);
    // Set the filename of the file reader
    fileReader->setFilename(fileName);

    // Now validate the filename
    if (fileReader->validateFilename()){
      setIntegerParam(ADSim_FileValid, 1);
      // Call the loadFile function
      status = loadFile();
      if (status == asynError){
        // Set the valid flag to false so we do not start bad acquisitions
        validFile = false;
      } else {
        // Set the valid flag to true, we can start acquisitions
        validFile = true;
      }
    } else {
      setIntegerParam(ADSim_FileValid, 0);
      status = asynError;
    }
  }

  // Do callbacks so higher layers see any changes
  callParamCallbacks(addr, addr);

  if (status){
    epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                  "%s:%s: status=%d, function=%d, value=%s",
                  driverName, functionName, status, function, value);
  } else {
    asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, value=%s\n",
              driverName, functionName, function, value);
  }
  *nActual = nChars;
  return status;
}

/** Read an image from the HDF5 file into the NDArray pointer.
  * \param[in] index used to determine which array within the selected dataset
  *            will be used as the image for this frame.
  */
asynStatus SimHDF5Detector::readImage(int index)
{
  int status = asynSuccess;
  int ndims=0;
  size_t dims[2];
  int itype;
  int width, height;
  int minX, minY;
  NDDataType_t dataType;
  int dsetIndex = 0;
  int xdim, ydim;
  const char *functionName = "readImage";

  // Get the datatype
  status |= getIntegerParam(NDDataType, &itype);
  dataType = (NDDataType_t)itype;
  // Get the dimensions
  status |= getIntegerParam(NDArraySizeX, &width);
  status |= getIntegerParam(NDArraySizeY, &height);
  // Get the offsets
  status |= getIntegerParam(ADMinX, &minX);
  status |= getIntegerParam(ADMinY, &minY);

  ndims = 2;
  dims[0] = width;
  dims[1] = height;

  // Release the previous array if necessary
  if (this->pRaw != NULL){
    this->pRaw->release();
  }
  // Allocate the new array
  this->pRaw = this->pNDArrayPool->alloc(ndims, dims, dataType, 0, NULL);

  if (!this->pRaw){
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: error allocating raw buffer\n",
              driverName, functionName);
    status = asynError;
  }

  if (status == asynSuccess){
    // First read the dataset index, selected dims and get the dataset info
    getIntegerParam(ADSim_DsetIndex, &dsetIndex);
    dsetIndex--;

    std::vector<int> dims = fileReader->getDatasetDimensions(fileReader->getDatasetKeys()[dsetIndex]);
    std::stringstream ss;
    ss << "%s:%s: Dimensions [";
    for (unsigned int i = 0; i < dims.size(); i++){
      ss << dims[i];
      if (i != dims.size()-1){
        ss << ", ";
      }
    }
    ss << "]\n";
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
              ss.str().c_str(),
              driverName, functionName);

    getIntegerParam(ADSim_XDim, &xdim);
    getIntegerParam(ADSim_YDim, &ydim);
    // The xdim and ydim values need to be converted to zero indexed
    xdim--;
    ydim--;

    // We need to calculate the offsets in the non-image dimensions
    if (dims.size() > 2){
      int cindex = index;
      int indexes[dims.size()-2];
      int ci = 0;
      int remainder = 0;
      int quotient = 0;
      for (int i = dims.size()-1; i >= 0; i--){
        if (i != xdim && i != ydim){
          quotient = cindex / dims[i];
          remainder = cindex - (quotient * dims[i]);
          cindex = quotient;
          // Work out the index in this dimension
          indexes[dims.size() - 3 - ci] = remainder;
          ci++;
        }
      }
      ss.str("");
      ss << "%s:%s: Indexes [";
      for (unsigned int i = 0; i < (dims.size()-2); i++){
        ss << indexes[i];
        if (i != dims.size()-3){
          ss << ", ";
        }
      }
      ss << "]\n";
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                ss.str().c_str(),
                driverName, functionName);

      // Read out the image into the array
      fileReader->readFromDataset(fileReader->getDatasetKeys()[dsetIndex], minX, minY, width, height, xdim, ydim, indexes, this->pRaw->pData);
    }
  }
  return (asynStatus)status;
}

/** Load the HDF5 file specified by the filename parameter.
 *
 * Validation checks are carried out before the file is loaded.
  */
asynStatus SimHDF5Detector::loadFile()
{
  // Retrieve the filename
  // Check the file exists
  // Open the file
  char *fileName = new char[MAX_FILENAME_LEN];
  fileName[MAX_FILENAME_LEN - 1] = '\0';
  asynStatus status = asynSuccess;
  const char *functionName = "loadFile";

  // Validate once more the filename
  if (fileReader->validateFilename()){
    setIntegerParam(ADSim_FileValid, 1);
  } else {
    setIntegerParam(ADSim_FileValid, 0);
    status = asynError;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: could not validate the file, is it in HDF5 format?\n",
              driverName, functionName);
  }

  // If the file is valid then read it in
  if (status == asynSuccess){
    // Read in the file
    fileReader->loadFile();

    // Verify there are datasets present
    std::vector<std::string> datasets = fileReader->getDatasetKeys();

    // Set the number of datasets parameter
    setIntegerParam(ADSim_NumOfDsets, datasets.size());

    // Set the current dataset index to 1
    // Note the parameter is not zero indexed!
    setIntegerParam(ADSim_DsetIndex, 1);

    if (datasets.size() > 0){
      // Update the dataset information
      readDatasetInfo();
    }
  }
  return status;
}

/** Read dataset information from the loaded HDF5 file.
 *
 * The dataset index parameter <b>ADSim_DsetIndex</b> is used to
 * select the specific dataset that will be used as the data source.
 * The information relating to that dataset is then read and processed.
  */
asynStatus SimHDF5Detector::readDatasetInfo()
{
  int dsetIndex = 0;
  asynStatus status = asynSuccess;
  const char *functionName = "readDatasetInfo";

  getIntegerParam(ADSim_DsetIndex, &dsetIndex);
  if (dsetIndex > (int)fileReader->getDatasetKeys().size() || dsetIndex < 1){
    status = asynError;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: Invalid dataset index, dataset not read\n",
              driverName, functionName);
  } else {
    dsetIndex--;

    // Set the dataset name
    setStringParam(ADSim_DsetName, fileReader->getDatasetKeys()[dsetIndex].c_str());

    std::vector<int> dims = fileReader->getDatasetDimensions(fileReader->getDatasetKeys()[dsetIndex]);
    // Set the number of available dimensions
    setIntegerParam(ADSim_DsetNumDims, dims.size());
    // For each dimension set the size of the dimension
    if (dims.size() > 0){
      setIntegerParam(ADSim_DsetDim1, dims[0]);
    } else {
      setIntegerParam(ADSim_DsetDim1, -1);
    }
    if (dims.size() > 1){
      setIntegerParam(ADSim_DsetDim2, dims[1]);
    } else {
      setIntegerParam(ADSim_DsetDim2, -1);
    }
    if (dims.size() > 2){
      setIntegerParam(ADSim_DsetDim3, dims[2]);
    } else {
      setIntegerParam(ADSim_DsetDim3, -1);
    }
    if (dims.size() > 3){
      setIntegerParam(ADSim_DsetDim4, dims[3]);
    } else {
      setIntegerParam(ADSim_DsetDim4, -1);
    }
    if (dims.size() > 4){
      setIntegerParam(ADSim_DsetDim5, dims[4]);
    } else {
      setIntegerParam(ADSim_DsetDim5, -1);
    }
    if (dims.size() > 5){
      setIntegerParam(ADSim_DsetDim6, dims[5]);
    } else {
      setIntegerParam(ADSim_DsetDim6, -1);
    }

    // Select XDim and YDim values that are the last and second last vector items
    // Note the parameters are not zero indexed but one!
    setIntegerParam(ADSim_XDim, dims.size());
    setIntegerParam(ADSim_YDim, dims.size()-1);

    status = updateSourceImage();
  }

  return status;
}

/** Update the detector parameters with the selected dataset info.
 *
 * The detector parameters for image size, array bytes and data type
 * are updated by this method.
 */
asynStatus SimHDF5Detector::updateSourceImage()
{
  int dsetIndex = 0;
  int xdim = 0;
  int ydim = 0;
  asynStatus status = asynSuccess;
  const char *functionName = "updateSourceImage";

  // First read the dataset index, selected dims and get the dataset info
  getIntegerParam(ADSim_DsetIndex, &dsetIndex);
  if (dsetIndex > (int)fileReader->getDatasetKeys().size()){
    status = asynError;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: Invalid dataset index, dataset not read\n",
              driverName, functionName);
  } else {
    dsetIndex--;

    std::vector<int> dims = fileReader->getDatasetDimensions(fileReader->getDatasetKeys()[dsetIndex]);
    getIntegerParam(ADSim_XDim, &xdim);
    getIntegerParam(ADSim_YDim, &ydim);
    if (xdim > (int)dims.size() || ydim > (int)dims.size() || xdim == ydim || xdim < 1 || ydim < 1){
      status = asynError;
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: Invalid dimension indexes given\n",
                driverName, functionName);
    } else {
      // The xdim and ydim values need to be converted to zero indexed
      xdim--;
      ydim--;

      // Set the sensor size to the selected dimensions
      setIntegerParam(ADMaxSizeX, dims[xdim]);
      setIntegerParam(ADMaxSizeY, dims[ydim]);
      setIntegerParam(ADSizeX, dims[xdim]);
      setIntegerParam(ADSizeY, dims[ydim]);
      setIntegerParam(ADMinX, 0);
      setIntegerParam(ADMinY, 0);

      // Get the data type for the dataset
      NDDataType_t type = fileReader->getDatasetType(fileReader->getDatasetKeys()[dsetIndex]);
      setIntegerParam(NDDataType, type);

      // Read the number of bytes for the datatype and set the NDArray parameters accordingly
      setIntegerParam(NDArraySizeX, dims[xdim]);
      setIntegerParam(NDArraySizeY, dims[ydim]);
      int bytes = 0;
      switch (type)
      {
        case NDInt8:
        case NDUInt8:
          bytes = 1;
          break;
        case NDInt16:
        case NDUInt16:
          bytes = 2;
          break;
        case NDInt32:
        case NDUInt32:
        case NDFloat32:
          bytes = 4;
          break;
        case NDFloat64:
          bytes = 8;
      }
      setIntegerParam(NDArraySize, dims[xdim]*dims[ydim]*bytes);
    }
  }
  return status;
}

/** Verify the specified sizes are within the bounds of the data source.
 *
 * Reads the minX, minY, sizeX, sizeY and verifies they do not exceed the
 * size of the source image.
 */
asynStatus SimHDF5Detector::verifySizes()
{
  asynStatus status = asynSuccess;
  int minX = 0;
  int sizeX = 0;
  int minY = 0;
  int sizeY = 0;
  int maxX = 0;
  int maxY = 0;
  const char *functionName = "verifySizes";

  // Get the min x,y of the image
  getIntegerParam(ADMinX, &minX);
  getIntegerParam(ADMinY, &minY);
  // Get the size of the image
  getIntegerParam(ADSizeX, &sizeX);
  getIntegerParam(ADSizeY, &sizeY);
  // Get the max size of the image
  getIntegerParam(ADMaxSizeX, &maxX);
  getIntegerParam(ADMaxSizeY, &maxY);

  // Check that minX+sizeX is not greater than maxX
  if ((minX+sizeX) > maxX){
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: Invalid X size requested minX: %d sizeX: %d\n",
              driverName, functionName, minX, sizeX);
    status = asynError;
  }
  if (status == asynSuccess){
    // Check that minY+sizeY is not greater than maxY
    if ((minY+sizeY) > maxY){
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: Invalid Y size requested minX: %d sizeX: %d\n",
                driverName, functionName, minX, sizeX);
      status = asynError;
    }
  }
  return status;
}

/** Calculated the NDArray size according to the requested ROI.
 *
 * Reads the sizeX and sizeY and calculates the size of the NDArray
 * according to the datatype.
 */
asynStatus SimHDF5Detector::setArraySizes()
{
  asynStatus status = asynSuccess;
  int sizeX = 0;
  int sizeY = 0;
  int type = 0;
  const char *functionName = "verifySizes";

  // Get the size of the image
  getIntegerParam(ADSizeX, &sizeX);
  getIntegerParam(ADSizeY, &sizeY);

  // Read the number of bytes for the datatype and set the NDArray parameters accordingly
  setIntegerParam(NDArraySizeX, sizeX);
  setIntegerParam(NDArraySizeY, sizeY);

  getIntegerParam(NDDataType, &type);

  int bytes = 0;
  switch (type)
  {
    case NDInt8:
    case NDUInt8:
      bytes = 1;
      break;
    case NDInt16:
    case NDUInt16:
      bytes = 2;
      break;
    case NDInt32:
    case NDUInt32:
    case NDFloat32:
      bytes = 4;
      break;
    case NDFloat64:
      bytes = 8;
  }
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: Calculating array sizeX: %d sizeY: %d bytes: %d\n",
            driverName, functionName, sizeX, sizeY, bytes);

  setIntegerParam(NDArraySize, sizeX*sizeY*bytes);
  return status;
}

/** Destructor.
 */
SimHDF5Detector::~SimHDF5Detector()
{
}

// Code required for iocsh registration of the SimHDF5Detector driver
extern "C"
{
  int SimHDF5DetectorConfig(const char *portName, int maxBuffers, size_t maxMemory, int priority, int stackSize)
  {
    new SimHDF5Detector(portName, maxBuffers, maxMemory, priority, stackSize);
    return asynSuccess;
  }
}

static const iocshArg SimHDF5DetectorConfigArg0 = {"portName", iocshArgString};
static const iocshArg SimHDF5DetectorConfigArg1 = {"Max number of NDArray buffers", iocshArgInt};
static const iocshArg SimHDF5DetectorConfigArg2 = {"maxMemory", iocshArgInt};
static const iocshArg SimHDF5DetectorConfigArg3 = {"priority", iocshArgInt};
static const iocshArg SimHDF5DetectorConfigArg4 = {"stackSize", iocshArgInt};

static const iocshArg * const SimHDF5DetectorConfigArgs[] =  {&SimHDF5DetectorConfigArg0,
                                                              &SimHDF5DetectorConfigArg1,
                                                              &SimHDF5DetectorConfigArg2,
                                                              &SimHDF5DetectorConfigArg3,
                                                              &SimHDF5DetectorConfigArg4};

static const iocshFuncDef configSimHDF5Detector = {"SimHDF5DetectorConfig", 5, SimHDF5DetectorConfigArgs};

static void configSimHDF5DetectorCallFunc(const iocshArgBuf *args)
{
    SimHDF5DetectorConfig(args[0].sval, args[1].ival, args[2].ival, args[3].ival, args[4].ival);
}

static void SimHDF5DetectorRegister(void)
{
    iocshRegister(&configSimHDF5Detector, configSimHDF5DetectorCallFunc);
}

epicsExportRegistrar(SimHDF5DetectorRegister);


