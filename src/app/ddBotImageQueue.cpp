#include "ddBotImageQueue.h"

#include <zlib.h>

#include <vtkIdTypeArray.h>
#include <vtkCellArray.h>
#include <vtkNew.h>

#include <multisense_utils/multisense_utils.hpp>
#include <vector>


//-----------------------------------------------------------------------------
ddBotImageQueue::ddBotImageQueue(QObject* parent) : QObject(parent)
{
  mBotParam = 0;
  mBotFrames = 0;
}

//-----------------------------------------------------------------------------
ddBotImageQueue::~ddBotImageQueue()
{
  foreach (CameraData* cameraData, mCameraData.values())
  {
    delete cameraData;
  }
}

//-----------------------------------------------------------------------------
bool ddBotImageQueue::initCameraData(const QString& cameraName, CameraData* cameraData)
{
  cameraData->mName = cameraName.toAscii().data();
  cameraData->mHasCalibration = true;
  cameraData->mImageMessage.utime = 0;

  cameraData->mCamTrans = bot_param_get_new_camtrans(mBotParam, cameraName.toAscii().data());
  if (!cameraData->mCamTrans)
  {
    printf("Failed to get BotCamTrans for camera: %s\n", qPrintable(cameraName));
    cameraData->mHasCalibration = false;
  }

  QString key = QString("cameras.") + cameraName + QString(".coord_frame");
  char* val = NULL;
  if (bot_param_get_str(mBotParam, key.toAscii().data(), &val) == 0)
  {
    cameraData->mCoordFrame = val;
    free(val);
  }
  else
  {
    printf("Failed to get coord_frame for camera: %s\n", qPrintable(cameraName));
    cameraData->mHasCalibration = false;
  }
  return true;
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::init(ddLCMThread* lcmThread,
                           const QString& botConfigFile) {
  if (botConfigFile.length()) {
    mBotParam = bot_param_new_from_file(botConfigFile.toAscii().data());
  } else {
    while (!mBotParam) {
      mBotParam = bot_param_new_from_server(
          lcmThread->lcmHandle()->getUnderlyingLCM(), 0);
    }
  }

  mBotFrames = bot_frames_get_global(lcmThread->lcmHandle()->getUnderlyingLCM(),
                                     mBotParam);

  // char** cameraNames = bot_param_get_all_camera_names(mBotParam);
  // for (int i = 0; cameraNames[i] != 0; ++i) {
  //   printf("camera: %s\n", cameraNames[i]);
  // }

  mLCM = lcmThread;
}

//-----------------------------------------------------------------------------
bool ddBotImageQueue::addCameraStream(const QString& channel)
{
  return this->addCameraStream(channel, channel, -1);
}

//-----------------------------------------------------------------------------
bool ddBotImageQueue::addCameraStream(const QString& channel, const QString& cameraName, int imageType)
{
  if (!this->mCameraData.contains(cameraName))
  {
    CameraData* cameraData = new CameraData;
    if (!this->initCameraData(cameraName, cameraData))
    {
      delete cameraData;
      return false;
    }

    this->mCameraData[cameraName] = cameraData;
    if (   imageType == bot_core::images_t::MASK_ZIPPED
        || imageType == bot_core::images_t::DISPARITY_ZIPPED
        || imageType == bot_core::images_t::DEPTH_MM_ZIPPED)
    {
      cameraData->mZlibCompression = true;
    }
  }

  this->mChannelMap[channel][imageType] = cameraName;

  if (!this->mSubscribers.contains(channel))
  {
    ddLCMSubscriber* subscriber = new ddLCMSubscriber(channel, this);

    if (imageType >= 0)
    {
      this->connect(subscriber, SIGNAL(messageReceived(const QByteArray&, const QString&)),
          SLOT(onImagesMessage(const QByteArray&, const QString&)), Qt::DirectConnection);
    }
    else
    {
      this->connect(subscriber, SIGNAL(messageReceived(const QByteArray&, const QString&)),
          SLOT(onImageMessage(const QByteArray&, const QString&)), Qt::DirectConnection);
    }

    this->mSubscribers[channel] = subscriber;
    mLCM->addSubscriber(subscriber);
  }


  return true;
}

//-----------------------------------------------------------------------------
QStringList ddBotImageQueue::getCameraNames() const {
  char** cameraNames = bot_param_get_all_camera_names(mBotParam);

  QStringList names;
  for (int i = 0; cameraNames[i] != 0; ++i) {
    names << cameraNames[i];
  }

  return names;
}

//-----------------------------------------------------------------------------
QStringList ddBotImageQueue::getBotFrameNames() const
{
  int nFrames = bot_frames_get_num_frames(mBotFrames);
  char** namesArray = bot_frames_get_frame_names(mBotFrames);

  QStringList names;
  for (int i = 0; i < nFrames; ++i)
  {
    names << namesArray[i];
  }

  return names;
}

//-----------------------------------------------------------------------------
int ddBotImageQueue::getTransform(const QString& fromFrame, const QString& toFrame, qint64 utime, vtkTransform* transform)
{
  if (!transform)
    {
    return 0;
    }

  double matx[16];
  int status = bot_frames_get_trans_mat_4x4_with_utime(mBotFrames, fromFrame.toAscii().data(),  toFrame.toAscii().data(), utime, matx);
  if (!status)
    {
    return 0;
    }

  vtkSmartPointer<vtkMatrix4x4> vtkmat = vtkSmartPointer<vtkMatrix4x4>::New();
  for (int i = 0; i < 4; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      vtkmat->SetElement(i, j, matx[i*4+j]);
    }
  }

  transform->SetMatrix(vtkmat);
  return status;
}

//-----------------------------------------------------------------------------
int ddBotImageQueue::getTransform(const QString& fromFrame, const QString& toFrame, vtkTransform* transform)
{
  if (!transform)
    {
    return 0;
    }

  double matx[16];
  int status = bot_frames_get_trans_mat_4x4(mBotFrames, fromFrame.toAscii().data(),  toFrame.toAscii().data(), matx);
  if (!status)
    {
    return 0;
    }

  vtkSmartPointer<vtkMatrix4x4> vtkmat = vtkSmartPointer<vtkMatrix4x4>::New();
  for (int i = 0; i < 4; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      vtkmat->SetElement(i, j, matx[i*4+j]);
    }
  }

  transform->SetMatrix(vtkmat);
  return status;
}

//-----------------------------------------------------------------------------
int ddBotImageQueue::getTransform(std::string from_frame, std::string to_frame,
                   Eigen::Isometry3d & mat, qint64 utime)
{
  double matx[16];
  int status = bot_frames_get_trans_mat_4x4_with_utime( mBotFrames, from_frame.c_str(),  to_frame.c_str(), utime, matx);
  if (!status)
    {
    return 0;
    }

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      mat(i,j) = matx[i*4+j];
    }
  }
  return status;
}

//-----------------------------------------------------------------------------
qint64 ddBotImageQueue::getImage(const QString& cameraName, vtkImageData* image)
{
  CameraData* cameraData = this->getCameraData(cameraName);
  if (!cameraData)
  {
    return 0;
  }

  image->DeepCopy(toVtkImage(cameraData));
  return cameraData->mImageMessage.utime;
}

//-----------------------------------------------------------------------------
qint64 ddBotImageQueue::getCurrentImageTime(const QString& cameraName)
{
  CameraData* cameraData = this->getCameraData(cameraName);
  if (!cameraData)
  {
    return 0;
  }

  return cameraData->mImageMessage.utime;
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::colorizePoints(const QString& cameraName, vtkPolyData* polyData)
{
  CameraData* cameraData = this->getCameraData(cameraName);
  if (!cameraData)
  {
    return;
  }

  colorizePoints(polyData, cameraData);
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::computeTextureCoords(const QString& cameraName, vtkPolyData* polyData)
{
  CameraData* cameraData = this->getCameraData(cameraName);
  if (!cameraData)
  {
    return;
  }

  this->computeTextureCoords(polyData, cameraData);
}

//-----------------------------------------------------------------------------
QList<double> ddBotImageQueue::getCameraFrustumBounds(const QString& cameraName)
{
  CameraData* cameraData = this->getCameraData(cameraName);
  if (!cameraData)
  {
    return QList<double>();
  }

  return this->getCameraFrustumBounds(cameraData);
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::getCameraProjectionTransform(const QString& cameraName, vtkTransform* transform)
{
  if (!transform)
  {
    return;
  }

  transform->Identity();

  CameraData* cameraData = this->getCameraData(cameraName);
  if (!cameraData)
  {
    return;
  }

  if (!cameraData->mHasCalibration)
  {
    return;
  }

  QMutexLocker locker(&cameraData->mMutex);

  double K00 = bot_camtrans_get_focal_length_x(cameraData->mCamTrans);
  double K11 = bot_camtrans_get_focal_length_y(cameraData->mCamTrans);
  double K01 = bot_camtrans_get_skew(cameraData->mCamTrans);
  double K02 = bot_camtrans_get_principal_x(cameraData->mCamTrans);
  double K12 = bot_camtrans_get_principal_y(cameraData->mCamTrans);

  vtkSmartPointer<vtkMatrix4x4> vtkmat = vtkSmartPointer<vtkMatrix4x4>::New();

  vtkmat->SetElement(0, 0, 1);
  vtkmat->SetElement(1, 1, 1);
  vtkmat->SetElement(2, 2, 1);
  vtkmat->SetElement(3, 3, 1);
  vtkmat->SetElement(0, 0, K00);
  vtkmat->SetElement(1, 1, K11);
  vtkmat->SetElement(0, 1, K01);
  //vtkmat->SetElement(0, 2, K02);
  //vtkmat->SetElement(1, 2, K12);

  transform->SetMatrix(vtkmat);
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::getBodyToCameraTransform(const QString& cameraName, vtkTransform* transform)
{
  if (!transform)
  {
    return;
  }

  transform->Identity();

  CameraData* cameraData = this->getCameraData(cameraName);
  if (!cameraData)
  {
    return;
  }

  QMutexLocker locker(&cameraData->mMutex);
  Eigen::Isometry3d mat = cameraData->mBodyToCamera;
  vtkSmartPointer<vtkMatrix4x4> vtkmat = vtkSmartPointer<vtkMatrix4x4>::New();
  for (int i = 0; i < 4; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      vtkmat->SetElement(i, j, mat(i,j));
    }
  }
  transform->SetMatrix(vtkmat);
}

//-----------------------------------------------------------------------------
ddBotImageQueue::CameraData* ddBotImageQueue::getCameraData(const QString& cameraName)
{
  return this->mCameraData.value(cameraName, NULL);
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::onImagesMessage(const QByteArray& data, const QString& channel)
{
  bot_core::images_t& message = this->mImagesMessageMap[channel];
  message.decode(data.data(), 0, data.size());

  const QMap<int, QString> cameraNameMap = mChannelMap[channel];

  for (QMap<int, QString>::const_iterator itr = cameraNameMap.constBegin(); itr != cameraNameMap.end(); ++itr)
  {
    int imageType = itr.key();
    const QString& cameraName = itr.value();

    bot_core::image_t* imageMessage = 0;
    for (int i = 0; i < message.n_images; ++i)
    {
      if (message.image_types[i] == imageType)
      {
        imageMessage = &message.images[i];
        break;
      }
    }

    if (!imageMessage)
    {
      return;
    }

    CameraData* cameraData = this->getCameraData(cameraName);

    QMutexLocker locker(&cameraData->mMutex);
    cameraData->mImageMessage = *imageMessage;
    cameraData->mImageBuffer.clear();

    if (cameraData->mHasCalibration)
    {
      this->getTransform("local", cameraData->mCoordFrame, cameraData->mLocalToCamera, cameraData->mImageMessage.utime);
      //this->getTransform("utorso", cameraData->mCoordFrame, cameraData->mBodyToCamera, cameraData->mImageMessage.utime);
    }

    //printf("got image %s: %d %d\n", cameraData->mName.c_str(), cameraData->mImageMessage.width, cameraData->mImageMessage.height);
  }
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::onImageMessage(const QByteArray& data, const QString& channel)
{
  const QString& cameraName = mChannelMap[channel][-1];

  CameraData* cameraData = this->getCameraData(cameraName);

  int64_t prevTimestamp = cameraData->mImageMessage.utime;

  QMutexLocker locker(&cameraData->mMutex);
  cameraData->mImageMessage.decode(data.data(), 0, data.size());
  cameraData->mImageBuffer.clear();

  if (cameraData->mImageMessage.utime == 0)
  {
    cameraData->mImageMessage.utime = prevTimestamp + 1;
  }

  if (cameraData->mHasCalibration)
  {
    this->getTransform("local", cameraData->mCoordFrame, cameraData->mLocalToCamera, cameraData->mImageMessage.utime);
    //this->getTransform("utorso", cameraData->mCoordFrame, cameraData->mBodyToCamera, cameraData->mImageMessage.utime);
  }

  //printf("got image %s: %d %d\n", cameraData->mName.c_str(), cameraData->mImageMessage.width, cameraData->mImageMessage.height);
}

void ddBotImageQueue::openLCMFile(const QString& filename)
{
  std::string file = filename.toStdString();
  assert(pangolin::FileExists(file.c_str()));
  this->logFile = new lcm::LogFile(file, "r");
}

bool ddBotImageQueue::readNextImagesMessage()
{
  // returns false if there is no more messages in log
  // gets the next images_t message and calls onImagesMessage()
  std::string channel = "";
  const lcm::LogEvent* event = 0;

  while (channel != "OPENNI_FRAME") {
      event = logFile->readNextEvent();
      if (event==NULL){
        // this means we made it to end of the file
        return false;
      }
      channel = event->channel;
      //std::cout << "read event on channel: " << channel << std::endl;
  }

  // event->data is a void*
  char* data_char_ptr = (char*) event->data;
  QByteArray data = QByteArray(data_char_ptr, event->datalen);

  QString channel_qstr = QString::fromStdString(channel);
  this->onImagesMessage(data, channel_qstr);
  return true;
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkImageData> ddBotImageQueue::toVtkImage(CameraData* cameraData)
{
  QMutexLocker locker(&cameraData->mMutex);

  size_t w = cameraData->mImageMessage.width;
  size_t h = cameraData->mImageMessage.height;

  if (w == 0 || h == 0)
  {
    return vtkSmartPointer<vtkImageData>::New();
  }

  int nComponents = 3;
  int componentType = VTK_UNSIGNED_CHAR;

  int pixelFormat = cameraData->mImageMessage.pixelformat;
  bool isZlibCompressed = cameraData->mZlibCompression;

  if (pixelFormat == bot_core::image_t::PIXEL_FORMAT_INVALID)
  {
    pixelFormat = bot_core::image_t::PIXEL_FORMAT_LE_GRAY16;
    isZlibCompressed = true;
  }
  else if (pixelFormat == bot_core::image_t::PIXEL_FORMAT_GRAY
          && cameraData->mImageMessage.row_stride/w == 2)
  {
    pixelFormat = bot_core::image_t::PIXEL_FORMAT_LE_GRAY16;
  }


  if (!cameraData->mImageBuffer.size())
  {
    if (pixelFormat == bot_core::image_t::PIXEL_FORMAT_RGB)
    {
      cameraData->mImageBuffer = cameraData->mImageMessage.data;
    }
    else if (pixelFormat == bot_core::image_t::PIXEL_FORMAT_MJPEG)
    {
      cameraData->mImageBuffer.resize(w*h*3);
      jpeg_decompress_8u_rgb(cameraData->mImageMessage.data.data(), cameraData->mImageMessage.size, cameraData->mImageBuffer.data(), w, h, w*3);
    }
    else if (pixelFormat == bot_core::image_t::PIXEL_FORMAT_GRAY)
    {
      cameraData->mImageBuffer = cameraData->mImageMessage.data;
      nComponents = 1;
    }
    else if (pixelFormat == bot_core::image_t::PIXEL_FORMAT_LE_GRAY16)
    {

      if (isZlibCompressed)
      {
        cameraData->mImageBuffer.resize(w*h*2);
        unsigned long len = cameraData->mImageBuffer.size();
        uncompress(cameraData->mImageBuffer.data(), &len, cameraData->mImageMessage.data.data(), cameraData->mImageMessage.size);
      }
      else
      {
        cameraData->mImageBuffer = cameraData->mImageMessage.data;
      }

      nComponents = 1;
      componentType = VTK_UNSIGNED_SHORT;
    }
    else
    {
      std::cout << "unhandled image pixelformat " << pixelFormat << " for camera " << cameraData->mName.c_str() << std::endl;
      return vtkSmartPointer<vtkImageData>::New();
    }
  }

  vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();

  image->SetWholeExtent(0, w-1, 0, h-1, 0, 0);
  image->SetSpacing(1.0, 1.0, 1.0);
  image->SetOrigin(0.0, 0.0, 0.0);
  image->SetExtent(image->GetWholeExtent());
  image->SetNumberOfScalarComponents(nComponents);
  image->SetScalarType(componentType);
  image->AllocateScalars();

  unsigned char* outPtr = static_cast<unsigned char*>(image->GetScalarPointer(0, 0, 0));

  std::copy(cameraData->mImageBuffer.begin(), cameraData->mImageBuffer.end(), outPtr);

  return image;
}

namespace {

//----------------------------------------------------------------------------
vtkSmartPointer<vtkCellArray> NewVertexCells(vtkIdType numberOfVerts)
{
  vtkNew<vtkIdTypeArray> cells;
  cells->SetNumberOfValues(numberOfVerts*2);
  vtkIdType* ids = cells->GetPointer(0);
  for (vtkIdType i = 0; i < numberOfVerts; ++i)
    {
    ids[i*2] = 1;
    ids[i*2+1] = i;
    }

  vtkSmartPointer<vtkCellArray> cellArray = vtkSmartPointer<vtkCellArray>::New();
  cellArray->SetCells(numberOfVerts, cells.GetPointer());
  return cellArray;
}

//----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> PolyDataFromPointCloud(pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr cloud)
{
  vtkIdType nr_points = cloud->points.size();

  vtkNew<vtkPoints> points;
  points->SetDataTypeToFloat();
  points->SetNumberOfPoints(nr_points);

  vtkNew<vtkUnsignedCharArray> rgbArray;
  rgbArray->SetName("rgb_colors");
  rgbArray->SetNumberOfComponents(3);
  rgbArray->SetNumberOfTuples(nr_points);

  vtkIdType j = 0;    // true point index
  for (vtkIdType i = 0; i < nr_points; ++i)
  {
    // Check if the point is invalid
    if (!pcl_isfinite (cloud->points[i].x) ||
        !pcl_isfinite (cloud->points[i].y) ||
        !pcl_isfinite (cloud->points[i].z))
      continue;

    float point[3] = {cloud->points[i].x, cloud->points[i].y, cloud->points[i].z};
    unsigned char color[3] = {cloud->points[i].r, cloud->points[i].g, cloud->points[i].b};
    points->SetPoint(j, point);
    rgbArray->SetTupleValue(j, color);
    j++;
  }
  nr_points = j;
  points->SetNumberOfPoints(nr_points);
  rgbArray->SetNumberOfTuples(nr_points);

  vtkSmartPointer<vtkPolyData> polyData = vtkSmartPointer<vtkPolyData>::New();
  polyData->SetPoints(points.GetPointer());
  polyData->GetPointData()->AddArray(rgbArray.GetPointer());
  polyData->SetVerts(NewVertexCells(nr_points));
  return polyData;
}


};

namespace {

  void vtkToRGBImageMessage(vtkImageData* image, bot_core::image_t& msg)
  {
    int width = image->GetDimensions()[0];
    int height = image->GetDimensions()[1];
    int nComponents = image->GetNumberOfScalarComponents();

    msg.width = width;
    msg.height = height;
    msg.nmetadata = 0;
    msg.row_stride = nComponents * width;
    msg.pixelformat = bot_core::image_t::PIXEL_FORMAT_RGB;

    msg.data.resize(width*height*nComponents);
    msg.size = msg.data.size();
    memcpy(&msg.data[0], image->GetScalarPointer(), msg.size);
  }

  void vtkToCompressedDepthMessage(vtkImageData* image, bot_core::image_t& msg)
  {
    int width = image->GetDimensions()[0];
    int height = image->GetDimensions()[1];
    int nComponents = image->GetNumberOfScalarComponents();


    static std::vector<uint8_t> compressBuffer;

    uLongf sourceSize = width*height*nComponents*2;
    uLongf bufferSize = compressBound(sourceSize);

    if (compressBuffer.size() < bufferSize)
    {
      printf("--->resizing compress buffer to %lu\n", bufferSize);
      compressBuffer.resize(bufferSize);
    }

    uLongf compressedSize = compressBuffer.size();

    compress2(&compressBuffer[0], &compressedSize,
      static_cast<Bytef*>(image->GetScalarPointer()), sourceSize, Z_BEST_SPEED);

    msg.width = width;
    msg.height = height;
    msg.nmetadata = 0;
    msg.row_stride = 0;
    msg.pixelformat = bot_core::image_t::PIXEL_FORMAT_INVALID;
    msg.data.resize(compressedSize);
    msg.size = msg.data.size();
    memcpy(&msg.data[0], &compressBuffer[0], compressedSize);
  }

}


//-----------------------------------------------------------------------------
void ddBotImageQueue::publishRGBDImagesMessage(const QString& channel,
    vtkImageData* colorImage, vtkImageData* depthImage, qint64 utime)
{
  bot_core::images_t msg;
  msg.images.resize(2);
  msg.n_images = msg.images.size();
  msg.image_types.push_back(int(bot_core::images_t::LEFT));
  msg.image_types.push_back(int(bot_core::images_t::DEPTH_MM_ZIPPED));

  msg.utime = utime;
  msg.images[0].utime = utime;
  msg.images[1].utime = utime;

  vtkToRGBImageMessage(colorImage, msg.images[0]);
  vtkToCompressedDepthMessage(depthImage, msg.images[1]);
  mLCM->lcmHandle()->publish(channel.toLatin1().data(), &msg);
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::publishRGBImageMessage(const QString& channel, vtkImageData* image, qint64 utime)
{
  bot_core::image_t msg;
  msg.utime = utime;
  vtkToRGBImageMessage(image, msg);
  mLCM->lcmHandle()->publish(channel.toLatin1().data(), &msg);
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::getPointCloudFromImages(const QString& channel, vtkPolyData* polyData, int decimation, int removeSize, float rangeThreshold)
{
  if (!this->mImagesMessageMap.contains(channel))
  {
    printf("no images received on channel: %s\n", qPrintable(channel));
    return;
  }

  bot_core::images_t& msg = this->mImagesMessageMap[channel];

  // Read the camera calibration from params (including baseline:
  QString channel_left = channel + QString("_LEFT");
  CameraData* cameraData = this->getCameraData(channel_left);
  QString key = QString("coordinate_frames.") + channel + QString("_RIGHT.initial_transform.translation");
  double baseline = 0.07; // an approximate value for multisense
  if (!bot_param_get_double(mBotParam, key.toAscii().data(), &baseline) == 0){
    printf("MULTISENSE_CAMERA_RIGHT baseline not found\n");
    return;
  }
  cv::Mat_<double> Q_(4, 4, 0.0);
  Q_(0,0) = Q_(1,1) = 1.0;
  Q_(3,2) = 1.0 / baseline;
  Q_(0,3) = -bot_camtrans_get_principal_x( cameraData->mCamTrans ); // cx
  Q_(1,3) = -bot_camtrans_get_principal_y( cameraData->mCamTrans ); // cy
  Q_(2,3) = bot_camtrans_get_focal_length_x( cameraData->mCamTrans ); // fx
  Q_(3,3) = 0;//(stereo_params_.right.cx - stereo_params_.left.cx ) / baseline;

  static multisense_utils m;
  m.set_decimate(decimation);
  m.set_remove_size( removeSize );

  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZRGB>);
  m.unpack_multisense(&msg, Q_, cloud);

  if (rangeThreshold >= 0) {
    pcl::PassThrough<pcl::PointXYZRGB> pass;
    pass.setInputCloud (cloud);
    pass.setFilterFieldName ("z");
    pass.setFilterLimits (0.001, rangeThreshold);
    pass.filter(*cloud);
  }

  polyData->ShallowCopy(PolyDataFromPointCloud(cloud));
}

//-----------------------------------------------------------------------------
int ddBotImageQueue::projectPoints(const QString& cameraName, vtkPolyData* polyData)
{

  CameraData* cameraData = this->getCameraData(cameraName);

  if (!cameraData->mHasCalibration)
  {
    printf("Error: projectPoints, no calibration data for: %s\n", cameraData->mName.c_str());
    return -1;
  }

  const vtkIdType nPoints = polyData->GetNumberOfPoints();
  for (vtkIdType i = 0; i < nPoints; ++i)
  {
    Eigen::Vector3d ptLocal;
    polyData->GetPoint(i, ptLocal.data());

    //Eigen::Vector3d pt = cameraData->mLocalToCamera * ptLocal;
    //Eigen::Vector3d pt = cameraData->mBodyToCamera * ptLocal;

    Eigen::Vector3d pt = ptLocal;

    double in[] = {pt[0], pt[1], pt[2]};
    double pix[3];

    if (bot_camtrans_project_point(cameraData->mCamTrans, in, pix) == 0)
    {
      polyData->GetPoints()->SetPoint(i, pix);
    }
  }

  return 1;
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::colorizePoints(vtkPolyData* polyData, CameraData* cameraData)
{
  if (!cameraData->mHasCalibration)
  {
    printf("Error: colorizePoints, no calibration data for: %s\n", cameraData->mName.c_str());
    return;
  }

  QMutexLocker locker(&cameraData->mMutex);

  size_t w = cameraData->mImageMessage.width;
  size_t h = cameraData->mImageMessage.height;
  size_t buf_size = w*h*3;

  bool computeDist = false;
  if (cameraData->mName == "CAMERACHEST_LEFT" || cameraData->mName == "CAMERACHEST_RIGHT")
  {
    computeDist = true;
  }

  if (!cameraData->mImageBuffer.size())
  {
    if (cameraData->mImageMessage.pixelformat != bot_core::image_t::PIXEL_FORMAT_MJPEG)
    {
      printf("Error: expected PIXEL_FORMAT_MJPEG for camera %s\n", cameraData->mName.c_str());
      return;
    }

    cameraData->mImageBuffer.resize(buf_size);
    jpeg_decompress_8u_rgb(cameraData->mImageMessage.data.data(), cameraData->mImageMessage.size, cameraData->mImageBuffer.data(), w, h, w*3);
  }

  vtkSmartPointer<vtkUnsignedCharArray> rgb = vtkUnsignedCharArray::SafeDownCast(polyData->GetPointData()->GetArray("rgb"));
  if (!rgb)
  {
    rgb = vtkSmartPointer<vtkUnsignedCharArray>::New();
    rgb->SetName("rgb");
    rgb->SetNumberOfComponents(3);
    rgb->SetNumberOfTuples(polyData->GetNumberOfPoints());
    polyData->GetPointData()->AddArray(rgb);

    rgb->FillComponent(0, 255);
    rgb->FillComponent(1, 255);
    rgb->FillComponent(2, 255);
  }

  const vtkIdType nPoints = polyData->GetNumberOfPoints();
  for (vtkIdType i = 0; i < nPoints; ++i)
  {
    Eigen::Vector3d ptLocal;
    polyData->GetPoint(i, ptLocal.data());
    Eigen::Vector3d pt = cameraData->mLocalToCamera * ptLocal;

    double in[] = {pt[0], pt[1], pt[2]};
    double pix[3];
    if (bot_camtrans_project_point(cameraData->mCamTrans, in, pix) == 0)
    {
      int px = static_cast<int>(pix[0]);
      int py = static_cast<int>(pix[1]);

      if (px >= 0 && px < w && py >= 0 && py < h)
      {

        if (computeDist)
        {
          float u = pix[0] / (w-1);
          float v = pix[1] / (h-1);
          if  ( ((0.5 - u)*(0.5 - u) + (0.5 - v)*(0.5 -v)) > 0.2 )
          {
            continue;
          }
        }

        size_t bufIndex = w*py*3 + px*3;
        rgb->SetComponent(i, 0, cameraData->mImageBuffer[bufIndex + 0]);
        rgb->SetComponent(i, 1, cameraData->mImageBuffer[bufIndex + 1]);
        rgb->SetComponent(i, 2, cameraData->mImageBuffer[bufIndex + 2]);
      }
    }
  }
}

//-----------------------------------------------------------------------------
QList<double> ddBotImageQueue::unprojectPixel(const QString& cameraName, int px, int py)
{
  double xyz[3];
  CameraData* cameraData = this->getCameraData(cameraName);
  if (!cameraData)
  {
    return QList<double>();
  }

  bot_camtrans_unproject_pixel(cameraData->mCamTrans, px, py, xyz);
  QList<double> result;
  result << xyz[0] << xyz[1] << xyz[2];
  return result;
}

//-----------------------------------------------------------------------------
QList<double> ddBotImageQueue::getCameraFrustumBounds(CameraData* cameraData)
{
  // Check whether transform exists and return to avoid segfault
  if (!cameraData->mCamTrans)
    return QList<double>();

  double width = bot_camtrans_get_image_width(cameraData->mCamTrans);
  double height = bot_camtrans_get_image_width(cameraData->mCamTrans);
  double ray[4][3];

  bot_camtrans_unproject_pixel(cameraData->mCamTrans, 0, 0, &ray[0][0]);
  bot_camtrans_unproject_pixel(cameraData->mCamTrans, width, 0, &ray[1][0]);
  bot_camtrans_unproject_pixel(cameraData->mCamTrans, width, height, &ray[2][0]);
  bot_camtrans_unproject_pixel(cameraData->mCamTrans, 0, height, &ray[3][0]);

  QList<double> rays;
  for (int i = 0; i < 4; ++i)
  {
    Eigen::Vector3d pt(&ray[i][0]);
    rays.append(pt[0]);
    rays.append(pt[1]);
    rays.append(pt[2]);
  }

  return rays;
}

//-----------------------------------------------------------------------------
void ddBotImageQueue::computeTextureCoords(vtkPolyData* polyData, CameraData* cameraData)
{
  if (!cameraData->mHasCalibration)
  {
    printf("Error: computeTextureCoords, no calibration data for: %s\n", cameraData->mName.c_str());
    return;
  }

  QMutexLocker locker(&cameraData->mMutex);

  size_t w = cameraData->mImageMessage.width;
  size_t h = cameraData->mImageMessage.height;

  bool computeDist = false;
  if (cameraData->mName == "CAMERACHEST_LEFT" || cameraData->mName == "CAMERACHEST_RIGHT")
  {
    computeDist = true;
  }

  std::string arrayName = "tcoords_" + cameraData->mName;
  vtkSmartPointer<vtkFloatArray> tcoords = vtkFloatArray::SafeDownCast(polyData->GetPointData()->GetArray(arrayName.c_str()));
  if (!tcoords)
  {
    tcoords = vtkSmartPointer<vtkFloatArray>::New();
    tcoords->SetName(arrayName.c_str());
    tcoords->SetNumberOfComponents(2);
    tcoords->SetNumberOfTuples(polyData->GetNumberOfPoints());
    polyData->GetPointData()->AddArray(tcoords);

    tcoords->FillComponent(0, -1);
    tcoords->FillComponent(1, -1);
  }

  const vtkIdType nPoints = polyData->GetNumberOfPoints();
  for (vtkIdType i = 0; i < nPoints; ++i)
  {
    Eigen::Vector3d ptLocal;
    polyData->GetPoint(i, ptLocal.data());
    //Eigen::Vector3d pt = cameraData->mLocalToCamera * ptLocal;
    //Eigen::Vector3d pt = cameraData->mBodyToCamera * ptLocal;
    Eigen::Vector3d pt = ptLocal;

    double in[] = {pt[0], pt[1], pt[2]};
    double pix[3];
    if (bot_camtrans_project_point(cameraData->mCamTrans, in, pix) == 0)
    {
      float u = pix[0] / (w-1);
      float v = pix[1] / (h-1);

      //if (u >= 0 && u <= 1.0 && v >= 0 && v <= 1.0)
      //{
        //if (computeDist &&  ((0.5 - u)*(0.5 - u) + (0.5 - v)*(0.5 -v)) > 0.14)
        //{
        //  continue;
        //}
        tcoords->SetComponent(i, 0, u);
        tcoords->SetComponent(i, 1, v);
      //}
    }
  }
}
