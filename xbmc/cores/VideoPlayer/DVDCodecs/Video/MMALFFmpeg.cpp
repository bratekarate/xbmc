/*
 *      Copyright (C) 2016 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "system.h"
#ifdef HAS_MMAL

#include <interface/mmal/util/mmal_default_components.h>

#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/MMALRenderer.h"
#include "../DVDCodecUtils.h"
#include "MMALFFmpeg.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "linux/RBP.h"
#include "settings/AdvancedSettings.h"

extern "C" {
#include "libavutil/imgutils.h"
}

using namespace MMAL;

//-----------------------------------------------------------------------------
// MMAL Buffers
//-----------------------------------------------------------------------------

#define CLASSNAME "CMMALYUVBuffer"

#define VERBOSE 0

CMMALYUVBuffer::CMMALYUVBuffer(uint32_t mmal_encoding, uint32_t width, uint32_t height, uint32_t aligned_width, uint32_t aligned_height, uint32_t size, int id)
  : CMMALBuffer(id)
{
  m_width = width;
  m_height = height;
  m_aligned_width = aligned_width;
  m_aligned_height = aligned_height;
  m_encoding = mmal_encoding;
  m_aspect_ratio = 0.0f;
  mmal_buffer = nullptr;
  m_rendered = false;
  m_stills = false;

  if (size == 0)
  {
    m_geo = g_RBP.GetFrameGeometry(m_encoding, aligned_width, aligned_height);
    const unsigned int size_y = m_geo.stride_y * m_geo.height_y;
    const unsigned int size_c = m_geo.stride_c * m_geo.height_c;
    m_size = (size_y + size_c * m_geo.planes_c) * m_geo.stripes;
  }
  else
    m_size = size;
  assert(m_size > 0);
  gmem = new CGPUMEM(m_size, true);
  if (gmem)
    gmem->m_opaque = (void *)this;
  else
    CLog::Log(LOGERROR, "%s::%s GCPUMEM(%d) failed", CLASSNAME, __FUNCTION__, m_size);
  if (VERBOSE && g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s buf:%p gmem:%p mmal:%p %dx%d (%dx%d) size:%d %.4s", CLASSNAME, __FUNCTION__, this, gmem, mmal_buffer, m_width, m_height, m_aligned_width, m_aligned_height, gmem->m_numbytes, (char *)&m_encoding);
}

CMMALYUVBuffer::~CMMALYUVBuffer()
{
  if (VERBOSE && g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s buf:%p gmem:%p mmal:%p %dx%d (%dx%d) size:%d %.4s", CLASSNAME, __FUNCTION__, this, gmem, mmal_buffer, m_width, m_height, m_aligned_width, m_aligned_height, gmem->m_numbytes, (char *)&m_encoding);
  delete gmem;
  gmem = nullptr;
}

//-----------------------------------------------------------------------------
// MMAL Decoder
//-----------------------------------------------------------------------------

#undef CLASSNAME
#define CLASSNAME "CDecoder"

CDecoder::CDecoder(CProcessInfo &processInfo, CDVDStreamInfo &hints) : m_processInfo(processInfo), m_hints(hints)
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - create %p", CLASSNAME, __FUNCTION__, this);
  m_avctx = nullptr;
  m_pool = nullptr;
  m_gmem = nullptr;
}

CDecoder::~CDecoder()
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - destroy %p", CLASSNAME, __FUNCTION__, this);
}

long CDecoder::Release()
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - m_refs:%ld", CLASSNAME, __FUNCTION__, m_refs.load());
  return IHardwareDecoder::Release();
}

void CDecoder::FFReleaseBuffer(void *opaque, uint8_t *data)
{
  CGPUMEM *gmem = (CGPUMEM *)opaque;
  CMMALYUVBuffer *YUVBuffer = (CMMALYUVBuffer *)gmem->m_opaque;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG,"%s::%s buf:%p gmem:%p", CLASSNAME, __FUNCTION__, YUVBuffer, gmem);

  YUVBuffer->Release();
}

int CDecoder::FFGetBuffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
  ICallbackHWAccel* cb = static_cast<ICallbackHWAccel*>(avctx->opaque);
  CDecoder* dec = static_cast<CDecoder*>(cb->GetHWAccel());
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG,"%s::%s %dx%d format:%x:%x flags:%x", CLASSNAME, __FUNCTION__, frame->width, frame->height, frame->format, dec->m_fmt, flags);

  if ((avctx->codec && (avctx->codec->capabilities & AV_CODEC_CAP_DR1) == 0) || frame->format != dec->m_fmt)
  {
    assert(0);
    return avcodec_default_get_buffer2(avctx, frame, flags);
  }

  uint32_t mmal_format = 0;
  if (dec->m_fmt == AV_PIX_FMT_YUV420P)
    mmal_format = MMAL_ENCODING_I420;
  else if (frame->format == AV_PIX_FMT_SAND128)
    mmal_format = MMAL_ENCODING_YUVUV128;
  else if (dec->m_fmt == AV_PIX_FMT_ARGB)
    mmal_format = MMAL_ENCODING_ARGB;
  else if (dec->m_fmt == AV_PIX_FMT_RGBA)
    mmal_format = MMAL_ENCODING_RGBA;
  else if (dec->m_fmt == AV_PIX_FMT_ABGR)
    mmal_format = MMAL_ENCODING_ABGR;
  else if (dec->m_fmt == AV_PIX_FMT_BGRA)
    mmal_format = MMAL_ENCODING_BGRA;
  else if (dec->m_fmt == AV_PIX_FMT_RGB565LE)
    mmal_format = MMAL_ENCODING_RGB16;
  if (mmal_format ==  0)
    return -1;

  std::shared_ptr<CMMALPool> pool = std::dynamic_pointer_cast<CMMALPool>(dec->m_pool);
  pool->SetFormat(mmal_format, frame->width, frame->height, frame->width, frame->height, 0, dec->m_avctx);
  CMMALYUVBuffer *YUVBuffer = dynamic_cast<CMMALYUVBuffer *>(pool->GetBuffer(500));
  if (!YUVBuffer || !YUVBuffer->mmal_buffer || !YUVBuffer->gmem)
  {
    CLog::Log(LOGERROR,"%s::%s Failed to allocated buffer in time", CLASSNAME, __FUNCTION__);
    return -1;
  }

  CSingleLock lock(dec->m_section);
  CGPUMEM *gmem = YUVBuffer->gmem;
  AVBufferRef *buf = av_buffer_create((uint8_t *)gmem->m_arm, YUVBuffer->m_size, CDecoder::FFReleaseBuffer, gmem, AV_BUFFER_FLAG_READONLY);
  if (!buf)
  {
    CLog::Log(LOGERROR, "%s::%s av_buffer_create() failed", CLASSNAME, __FUNCTION__);
    YUVBuffer->Release();
    return -1;
  }

  for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
  {
    frame->buf[i] = NULL;
    frame->data[i] = NULL;
    frame->linesize[i] = 0;
  }

  if (dec->m_fmt == AV_PIX_FMT_YUV420P)
  {
    frame->buf[0] = buf;
    frame->linesize[0] = YUVBuffer->m_aligned_width;
    frame->linesize[1] = YUVBuffer->m_aligned_width>>1;
    frame->linesize[2] = YUVBuffer->m_aligned_width>>1;
    frame->data[0] = (uint8_t *)gmem->m_arm;
    frame->data[1] = frame->data[0] + YUVBuffer->m_aligned_width * YUVBuffer->m_aligned_height;
    frame->data[2] = frame->data[1] + (YUVBuffer->m_aligned_width>>1) * (YUVBuffer->m_aligned_height>>1);
  }
  else if (frame->format == AV_PIX_FMT_SAND128)
  {
    const unsigned int size_y = YUVBuffer->m_geo.stride_y * YUVBuffer->m_geo.height_y;
    const unsigned int size_c = YUVBuffer->m_geo.stride_c * YUVBuffer->m_geo.height_c;

    frame->buf[0] = buf;

    frame->linesize[0] = YUVBuffer->m_geo.stride_y;
    frame->linesize[1] = YUVBuffer->m_geo.stride_c;
    frame->linesize[2] = YUVBuffer->m_geo.stride_c;
    if (YUVBuffer->m_geo.stripes > 1)
        frame->linesize[3] = YUVBuffer->m_geo.height_y + YUVBuffer->m_geo.height_c;      // abuse: linesize[3] = stripe stride

    frame->data[0] = (uint8_t *)gmem->m_arm;
    frame->data[1] = frame->data[0] + size_y;
    if (YUVBuffer->m_geo.planes_c > 1)
        frame->data[2] = frame->data[1] + size_c;

    frame->extended_data = frame->data;
    // Leave extended buf alone
  }
  else if (dec->m_fmt == AV_PIX_FMT_BGR0)
  {
    frame->buf[0] = buf;
    frame->linesize[0] = YUVBuffer->m_aligned_width << 2;
    frame->data[0] = (uint8_t *)gmem->m_arm;
  }
  else if (dec->m_fmt == AV_PIX_FMT_RGB565LE)
  {
    frame->buf[0] = buf;
    frame->linesize[0] = YUVBuffer->m_aligned_width << 1;
    frame->data[0] = (uint8_t *)gmem->m_arm;
  }
  else assert(0);
  frame->extended_data = frame->data;
  // Leave extended buf alone

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG,"%s::%s buf:%p mmal:%p gmem:%p avbuf:%p:%p:%p", CLASSNAME, __FUNCTION__, YUVBuffer, YUVBuffer->mmal_buffer, gmem, frame->data[0], frame->data[1], frame->data[2]);

  return 0;
}


bool CDecoder::Open(AVCodecContext *avctx, AVCodecContext* mainctx, enum AVPixelFormat fmt)
{
  CSingleLock lock(m_section);

  CLog::Log(LOGNOTICE, "%s::%s - fmt:%d", CLASSNAME, __FUNCTION__, fmt);

  CLog::Log(LOGDEBUG, "%s::%s MMAL - source requires %d references", CLASSNAME, __FUNCTION__, avctx->refs);

  avctx->get_buffer2 = CDecoder::FFGetBuffer;
  mainctx->get_buffer2 = CDecoder::FFGetBuffer;

  m_avctx = mainctx;
  m_fmt = fmt;

  /* Create dummy component with attached pool */
  m_pool = std::make_shared<CMMALPool>(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, false, MMAL_NUM_OUTPUT_BUFFERS, 0, MMAL_ENCODING_I420, MMALStateFFDec);
  if (!m_pool)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create pool for decoder output", CLASSNAME, __func__);
    return false;
  }

  std::shared_ptr<CMMALPool> pool = std::dynamic_pointer_cast<CMMALPool>(m_pool);
  pool->SetDecoder(this);
  pool->SetProcessInfo(&m_processInfo);

  std::list<EINTERLACEMETHOD> deintMethods;
  deintMethods.push_back(EINTERLACEMETHOD::VS_INTERLACEMETHOD_AUTO);
  deintMethods.push_back(EINTERLACEMETHOD::VS_INTERLACEMETHOD_MMAL_ADVANCED);
  deintMethods.push_back(EINTERLACEMETHOD::VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF);
  deintMethods.push_back(EINTERLACEMETHOD::VS_INTERLACEMETHOD_MMAL_BOB);
  deintMethods.push_back(EINTERLACEMETHOD::VS_INTERLACEMETHOD_MMAL_BOB_HALF);
  m_processInfo.UpdateDeinterlacingMethods(deintMethods);

  return true;
}

CDVDVideoCodec::VCReturn CDecoder::Decode(AVCodecContext* avctx, AVFrame* frame)
{
  CSingleLock lock(m_section);

  if (frame)
  {
    if ((frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_SAND128 && frame->format != AV_PIX_FMT_BGR0 && frame->format != AV_PIX_FMT_RGB565LE) ||
        frame->buf[1] != nullptr || frame->buf[0] == nullptr)
    {
      CLog::Log(LOGERROR, "%s::%s frame format invalid format:%d buf:%p,%p", CLASSNAME, __func__, frame->format, frame->buf[0], frame->buf[1]);
      return CDVDVideoCodec::VC_ERROR;
    }
    AVBufferRef *buf = frame->buf[0];
    m_gmem = (CGPUMEM *)av_buffer_get_opaque(buf);
  }
  CDVDVideoCodec::VCReturn status = Check(avctx);
  if (status != CDVDVideoCodec::VC_NONE)
    return status;

  if (frame)
    return CDVDVideoCodec::VC_PICTURE;
  else
    return CDVDVideoCodec::VC_BUFFER;
}

bool CDecoder::GetPicture(AVCodecContext* avctx, VideoPicture* picture)
{
  CSingleLock lock(m_section);

  bool ret = ((ICallbackHWAccel*)avctx->opaque)->GetPictureCommon(picture);
  if (!ret || !m_gmem)
    return false;

  CMMALBuffer *buffer = static_cast<CMMALBuffer*>(m_gmem->m_opaque);
  picture->videoBuffer = dynamic_cast<CVideoBuffer *>(buffer);
  assert(buffer);
  assert(buffer->mmal_buffer);
  buffer->mmal_buffer->data = (uint8_t *)m_gmem->m_vc_handle;
  buffer->mmal_buffer->alloc_size = buffer->mmal_buffer->length = m_gmem->m_numbytes;
  buffer->m_stills = m_hints.stills;

  // need to flush ARM cache so GPU can see it (HEVC will have already done this)
  if (avctx->codec_id != AV_CODEC_ID_HEVC)
    m_gmem->Flush();

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - mmal:%p dts:%.3f pts:%.3f buf:%p gpu:%p", CLASSNAME, __FUNCTION__, buffer->mmal_buffer, 1e-6*picture->dts, 1e-6*picture->pts, buffer, m_gmem);
  return true;
}

CDVDVideoCodec::VCReturn CDecoder::Check(AVCodecContext* avctx)
{
  CSingleLock lock(m_section);
  return CDVDVideoCodec::VC_NONE;
}

unsigned CDecoder::GetAllowedReferences()
{
  return 6;
}

#endif
