#pragma once

/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
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

#include "DVDInputStream.h"
#include <list>
#include <memory>

extern "C"
{
#include <libbluray/bluray.h>
#include <libbluray/bluray-version.h>
#include <libbluray/keys.h>
#include <libbluray/overlay.h>
#include <libbluray/player_settings.h>
}

#define MAX_PLAYLIST_ID 99999
#define BD_EVENT_MENU_OVERLAY -1
#define BD_EVENT_MENU_ERROR   -2
#define BD_EVENT_ENC_ERROR    -3

class CDVDOverlayImage;
class DllLibbluray;
class IVideoPlayer;
class CDVDDemux;

class CDVDInputStreamBluray 
  : public CDVDInputStream
  , public CDVDInputStream::IDisplayTime
  , public CDVDInputStream::IChapter
  , public CDVDInputStream::IPosTime
  , public CDVDInputStream::IMenus
  , public CDVDInputStream::IExtentionStream
{
public:
  CDVDInputStreamBluray(IVideoPlayer* player, const CFileItem& fileitem);
  virtual ~CDVDInputStreamBluray();
  virtual bool Open();
  virtual void Close();
  virtual int Read(uint8_t* buf, int buf_size);
  virtual int64_t Seek(int64_t offset, int whence);
  virtual bool Pause(double dTime) { return false; };
  void Abort();
  virtual bool IsEOF();
  virtual int64_t GetLength();
  virtual int GetBlockSize() { return 6144; }
  virtual ENextStream NextStream();


  /* IMenus */
  virtual void ActivateButton()          { UserInput(BD_VK_ENTER); }
  virtual void SelectButton(int iButton)
  {
    if(iButton < 10)
      UserInput((bd_vk_key_e)(BD_VK_0 + iButton));
  }
  virtual int  GetCurrentButton()        { return 0; }
  virtual int  GetTotalButtons()         { return 0; }
  virtual void OnUp()                    { UserInput(BD_VK_UP); }
  virtual void OnDown()                  { UserInput(BD_VK_DOWN); }
  virtual void OnLeft()                  { UserInput(BD_VK_LEFT); }
  virtual void OnRight()                 { UserInput(BD_VK_RIGHT); }
  virtual void OnMenu();
  virtual void OnBack()
  {
    if(IsInMenu())
      OnMenu();
  }
  virtual void OnNext()                  {}
  virtual void OnPrevious()              {}
  virtual bool HasMenu();
  virtual bool IsInMenu();
  virtual bool OnMouseMove(const CPoint &point)  { return MouseMove(point); }
  virtual bool OnMouseClick(const CPoint &point) { return MouseClick(point); }
  virtual void SkipStill();
  virtual bool GetState(std::string &xmlstate)         { return false; }
  virtual bool SetState(const std::string &xmlstate)   { return false; }


  void UserInput(bd_vk_key_e vk);
  bool MouseMove(const CPoint &point);
  bool MouseClick(const CPoint &point);

  int GetChapter();
  int GetChapterCount();
  void GetChapterName(std::string& name, int ch=-1) {};
  int64_t GetChapterPos(int ch);
  bool SeekChapter(int ch);

  CDVDInputStream::IDisplayTime* GetIDisplayTime() override { return this; }
  int GetTotalTime() override;
  int GetTime() override;

  CDVDInputStream::IPosTime* GetIPosTime() override { return this; }
  bool PosTime(int ms);

  void GetStreamInfo(int pid, char* language);

  void OverlayCallback(const BD_OVERLAY * const);
#ifdef HAVE_LIBBLURAY_BDJ
  void OverlayCallbackARGB(const struct bd_argb_overlay_s * const);
#endif

  BLURAY_TITLE_INFO* GetTitleLongest();
  BLURAY_TITLE_INFO* GetTitleFile(const std::string& name);

  void ProcessEvent();
  CDVDDemux* GetExtentionDemux() override { return m_pMVCDemux; };
  bool HasExtention() override { return m_bMVCPlayback; }
  bool AreEyesFlipped() override { return m_bFlipEyes; }
  void DisableExtention() override;

protected:
  struct SPlane;

  void OverlayFlush(int64_t pts);
  void OverlayClose();
  static void OverlayClear(SPlane& plane, int x, int y, int w, int h);
  static void OverlayInit (SPlane& plane, int w, int h);
  bool ProcessItem(int playitem);

  bool OpenMVCDemux(int playItem);
  bool CloseMVCDemux();
  void SeekMVCDemux(int64_t time);

  IVideoPlayer*         m_player;
  DllLibbluray*       m_dll;
  BLURAY*             m_bd;
  BLURAY_TITLE_INFO*  m_title;
  uint32_t            m_playlist;
  uint32_t            m_clip;
  uint32_t            m_angle;
  bool                m_menu;
  bool                m_navmode;
  int m_dispTimeBeforeRead;
  int                 m_nTitles = -1;
  std::string         m_root;

  // MVC related members
  CDVDDemux*          m_pMVCDemux = nullptr;
  CDVDInputStream    *m_pMVCInput = nullptr;
  bool                m_bMVCPlayback = false;
  int                 m_nMVCSubPathIndex = 0;
  int                 m_nMVCClip = -1;
  bool                m_bFlipEyes = false;
  bool                m_bMVCDisabled = false;
  uint64_t            m_clipStartTime = 0;

  typedef std::shared_ptr<CDVDOverlayImage> SOverlay;
  typedef std::list<SOverlay>                 SOverlays;

  struct SPlane
  {
    SOverlays o;
    int       w;
    int       h;

    SPlane()
    : w(0)
    , h(0)
    {}
  };

  SPlane m_planes[2];
  enum EHoldState {
    HOLD_NONE = 0,
    HOLD_HELD,
    HOLD_DATA,
    HOLD_STILL,
    HOLD_ERROR,
    HOLD_EXIT
  } m_hold;
  BD_EVENT m_event;
#ifdef HAVE_LIBBLURAY_BDJ
  struct bd_argb_buffer_s m_argb;
#endif

  private:
    void SetupPlayerSettings();
};
