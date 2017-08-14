/** @file
  Driver to publish the Test Profile Library Protocol.

  Copyright (c) 2006 - 2017, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/TslInit.h>
#include <Protocol/TestProfileLibrary.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LibPrivate.h>

//
// Private Data definition for TestProfileLibrary protocol
//
#define TEST_PROFILE_PRIVATE_DATA_SIGNATURE   SIGNATURE_32('A','D','V','L')

typedef struct {
  UINT32                                    Signature;
  EFI_TEST_PROFILE_LIBRARY_PROTOCOL         TestProfile;
  EFI_DEVICE_PATH_PROTOCOL                  *DevicePath;
  CHAR16                                    *FilePath;
} TEST_PROFILE_PRIVATE_DATA;

#define TEST_PROFILE_PRIVATE_DATA_FROM_THIS(a)  \
  CR(a, TEST_PROFILE_PRIVATE_DATA, TestProfile, TEST_PROFILE_PRIVATE_DATA_SIGNATURE)

//
// Private Data definition for EFI_INI_FILE interface
//
#define EFI_INI_FILE_PRIVATE_DATA_SIGNATURE   SIGNATURE_32('I','N','I','F')

#define MAX_STRING_LEN      250
#define MAX_LINE_LEN        512

//
// Forward reference for pure ANSI compatibility
//
typedef struct _INI  INI;
typedef struct _COMMENTLINE  COMMENTLINE;

struct _INI {
  UINT32                          commentNo;
  CHAR8                           *ptrSection;
  CHAR8                           *ptrEntry;
  CHAR8                           *ptrValue;
  INI                             *ptrNext;
};


struct _COMMENTLINE {
  UINT32                          commentNo;
  CHAR8                           *ptrComment;
  COMMENTLINE                     *ptrNext;
};

typedef struct {
  UINT32                          Signature;
  EFI_INI_FILE                    Handle;
  EFI_DEVICE_PATH_PROTOCOL        *DevPath;
  CHAR16                          *FileName;
  INI                             *Head;
  INI                             *Tail;
  COMMENTLINE                     *CommentLineHead;
  BOOLEAN                         isUnicode;
  BOOLEAN                         Modified;
} EFI_INI_FILE_PRIVATE_DATA;

#define EFI_INI_FILE_PRIVATE_DATA_FROM_THIS(a)  \
  CR(a, EFI_INI_FILE_PRIVATE_DATA, Handle, EFI_INI_FILE_PRIVATE_DATA_SIGNATURE)

//
// Name and Description of EFI_TEST_PROFILE_LIBRARY_PROTOCOL
//
CHAR16 *gAtslName        = L"Test Profile Library";
CHAR16 *gAtslDescription = L"EFI Test Profile Library";

//
// Internal functions
//
CHAR8 *
AsciiDuplicateString (
  CHAR8  *str
)
{
  return (str == NULL) ? NULL : AllocateCopyPool (AsciiStrSize (str), str);
}

CHAR16 *
UnicodeDuplicateString (
  CHAR16      *str
)
{
  return (str == NULL) ? NULL : AllocateCopyPool (StrSize (str), str);
}

INTN
TestProfile_wcstombs(
  CHAR8       *s,
  CHAR16      *pwcs,
  UINTN       n
  )
{
  EFI_STATUS  Status;

  Status = UnicodeStrToAsciiStrS(pwcs, s, n);
  if (EFI_ERROR (Status)) {
    return -1;
  }
  return AsciiStrnLenS (s, n);
}

INTN
TestProfile_mbstowcs(
  CHAR16      *pwcs,
  CHAR8       *s,
  UINTN       n
  )
{
  EFI_STATUS  Status;

  Status = AsciiStrToUnicodeStrS(s, pwcs, n);
  if (EFI_ERROR (Status)) {
    return -1;
  }
  return StrnLenS (pwcs, n);
}

VOID
_freePointer (
  IN EFI_INI_FILE_PRIVATE_DATA    *IniFile
  )
/*++

Routine Description:

  Free all content of an ini file.

--*/
{
  INI         *ptrCur;
  INI         *ptrTmp;
  COMMENTLINE *ptrCommentCur;
  COMMENTLINE *ptrCommentTmp;

  ptrCur = IniFile->Head;

  while (ptrCur != NULL) {
    FreePool (ptrCur->ptrSection);
    FreePool (ptrCur->ptrEntry);
    FreePool (ptrCur->ptrValue);
    ptrCur = ptrCur->ptrNext;
  }

  while (IniFile->Head != NULL) {
    ptrTmp = IniFile->Head->ptrNext;
    FreePool (IniFile->Head);
    IniFile->Head = ptrTmp;
  }
  IniFile->Tail = NULL;

  ptrCommentCur = IniFile->CommentLineHead;

  while (ptrCommentCur != NULL) {
    FreePool (ptrCommentCur->ptrComment);
    ptrCommentCur = ptrCommentCur->ptrNext;
  }

  while (IniFile->CommentLineHead != NULL) {
    ptrCommentTmp = IniFile->CommentLineHead->ptrNext;
    FreePool (IniFile->CommentLineHead);
    IniFile->CommentLineHead = ptrCommentTmp;
  }
}

VOID
_getcomment(
  IN EFI_INI_FILE_PRIVATE_DATA    *IniFile,
  IN CHAR8                        *ptrStr,
  OUT UINT32                      *commentNo
  )
/*++

Routine Description:

  Get comment line.

--*/
{
  static COMMENTLINE *ptrCommentLineCurrent = NULL;
  COMMENTLINE        *ptrCommentLineNew;
  UINT32             Index;
  UINT32             Length;

  //
  // skip '\n' & '\r' at end of comment line
  //
  Length = (UINT32) AsciiStrLen (ptrStr);
  for (Index = Length; Index > 0; Index --) {
    if (ptrStr[Index - 1] != '\n' && ptrStr[Index - 1] != '\r') {
      break;
    }
  }
  ptrStr[Index] = '\0';

  ptrCommentLineNew = (COMMENTLINE *) AllocatePool (sizeof(COMMENTLINE));
  ptrCommentLineNew->ptrComment = (CHAR8 *) AllocateZeroPool (AsciiStrLen(ptrStr) + 1);

  ptrCommentLineNew->commentNo = *commentNo;
  AsciiStrCpy (ptrCommentLineNew->ptrComment, ptrStr);

  if (IniFile->CommentLineHead == NULL) {
    IniFile->CommentLineHead = ptrCommentLineNew;
    IniFile->CommentLineHead->ptrNext = NULL;
    ptrCommentLineCurrent = IniFile->CommentLineHead;
  } else {
    ptrCommentLineNew->ptrNext = NULL;
    ptrCommentLineCurrent->ptrNext = ptrCommentLineNew;
    ptrCommentLineCurrent = ptrCommentLineNew;
  }
}

CHAR8 *_alltrim(
  CHAR8 *ptrStr
  )
/*++

Routine Description:

  Remove the space from both side of the string and the ending '\n' or '\r'.

--*/
{
  CHAR8   *tmp = ptrStr;
  UINT32  Length;
  UINT32  Index;

  //
  // skip the ' ' at beginning of string
  //
  while (*tmp == ' ') {
    tmp ++;
  }

  //
  // skip '\n' & ' ' & '\r' at end of string
  //
  Length = (UINT32) AsciiStrLen (tmp);
  for (Index = Length; Index > 0; Index --) {
    if (tmp[Index - 1] != '\n' && tmp[Index - 1] != ' ' && tmp[Index - 1] != '\r') {
      break;
    }
  }

  tmp[Index] = '\0';
  CopyMem(ptrStr, tmp, Index + 1);

  return ptrStr;
}

VOID
_additem (
  IN EFI_INI_FILE_PRIVATE_DATA    *IniFile,
  IN INI                          *ptrItem
  )
/*++

Routine Description:

  Insert new item to the end of ini table .

--*/
{
  if (IniFile->Head == NULL) {
    IniFile->Head = ptrItem;
    IniFile->Tail = ptrItem;
    IniFile->Head->ptrNext = NULL;
  } else {
    ptrItem->ptrNext = NULL;
    IniFile->Tail->ptrNext = ptrItem;
    IniFile->Tail = ptrItem;
  }
}


VOID
_getsection (
  IN EFI_INI_FILE_PRIVATE_DATA    *IniFile,
  IN CHAR8                        *ptrStr,
  OUT CHAR8                       *ptrSection,
  OUT UINT32                      *commentNo
  )
/*++

Routine Description:

  Parse one line and generate section.

--*/
{
  CHAR8 *p, *q;
  INI   *ptrItem;

  p = AsciiStrStr (ptrStr, "[");
  q = AsciiStrStr (ptrStr, "]");

  *q = '\0' ;

  _alltrim (++p);

  if (AsciiStrLen (p) <= MAX_STRING_LEN) {
    if (*p == '\0') {
      AsciiStrCpy (ptrSection, "UNKNOWN");
    } else {
      AsciiStrCpy (ptrSection, p);
    }
  } else {
    AsciiStrnCpy (ptrSection, p, MAX_STRING_LEN);
    ptrSection[MAX_STRING_LEN] = '\0';
  }

  ptrItem = (INI *)AllocatePool (sizeof(INI));

  ptrItem->ptrSection = (CHAR8 *) AllocateZeroPool (AsciiStrLen(ptrSection) + 1);
  ptrItem->ptrEntry   = (CHAR8 *) AllocateZeroPool (1);
  ptrItem->ptrValue   = (CHAR8 *) AllocateZeroPool (1);

  ptrItem->commentNo  = *commentNo;
  AsciiStrCpy (ptrItem->ptrSection, ptrSection);
  AsciiStrCpy (ptrItem->ptrEntry, "");
  AsciiStrCpy (ptrItem->ptrValue, "");

  (*commentNo) ++;

  _additem (IniFile, ptrItem);
}

VOID
_getentry (
  IN CHAR8                        *ptrStr,
  IN CHAR8                        *ptrEntry,
  OUT CHAR8                       *ptrValue
  )
/*++

Routine Description:

  Parse one line and generate the entry and value.

--*/
{
  CHAR8 *p, *p2;

  p = AsciiStrStr (ptrStr, "=");

  *p = '\0';

  _alltrim (ptrStr);

  if (AsciiStrLen (ptrStr) <= MAX_STRING_LEN) {
    if (*ptrStr == '\0') {
      AsciiStrCpy (ptrEntry, "UNKNOWN");
    } else {
      AsciiStrCpy (ptrEntry, ptrStr);
    }
  } else {
    AsciiStrnCpy (ptrEntry, ptrStr, MAX_STRING_LEN);
    ptrEntry[MAX_STRING_LEN] = '\0';
  }

  _alltrim (++ p);
  p2 = AsciiStrStr (p, "#");
  if (p2 != NULL) {
    *p2 = '\0';
    _alltrim (p);
  }

  if (AsciiStrLen (p) <= MAX_STRING_LEN) {
    AsciiStrCpy (ptrValue, p);
  } else {
    AsciiStrnCpy (ptrValue, p, MAX_STRING_LEN);
    ptrValue[MAX_STRING_LEN] = '\0';
  }
}

VOID
_prosessLine (
  IN EFI_INI_FILE_PRIVATE_DATA    *IniFile,
  IN CHAR8                        *ptrLine,
  OUT CHAR8                       *ptrSection,
  OUT BOOLEAN                     *isSectionGot,
  OUT UINT32                      *commentNo
  )
/*++

Routine Description:

  Parse a line.

--*/
{
  CHAR8 ptrEntry[MAX_STRING_LEN + 1];
  CHAR8 ptrValue[MAX_STRING_LEN + 1];
  INI   *ptrItem;

  _alltrim (ptrLine);

  if (*ptrLine == '#') {
    // it's a comment line
    _getcomment (IniFile, ptrLine, commentNo);
  } else if ((*ptrLine == '[') && (AsciiStrStr (ptrLine, "]") != NULL)) {
    // it's a section head
    _getsection (IniFile, ptrLine, ptrSection, commentNo);
    *isSectionGot = TRUE;
  } else if (AsciiStrStr (ptrLine, "=") != NULL) {
    _getentry (ptrLine, ptrEntry, ptrValue);

    if (*isSectionGot == TRUE) {
      ptrItem = (INI *)AllocatePool (sizeof(INI));
      ptrItem->ptrSection = (CHAR8 *) AllocateZeroPool (AsciiStrLen(ptrSection) + 1);
      ptrItem->ptrEntry   = (CHAR8 *) AllocateZeroPool (AsciiStrLen(ptrEntry) + 1);
      ptrItem->ptrValue   = (CHAR8 *) AllocateZeroPool (AsciiStrLen(ptrValue) + 1);

      ptrItem->commentNo = *commentNo;
      AsciiStrCpy (ptrItem->ptrSection, ptrSection);
      AsciiStrCpy (ptrItem->ptrEntry, ptrEntry);
      AsciiStrCpy (ptrItem->ptrValue, ptrValue);

      (*commentNo) ++;

      _additem (IniFile, ptrItem);
    }
  }
}

INI *
_getNextSection (
  IN INI                          *CurSection
)
/*++

Routine Description:

  Search for the next section in the ini table

--*/
{
  INI   *ptrCur;

  ptrCur = CurSection->ptrNext;
  while (ptrCur != NULL) {
    if (*(ptrCur->ptrEntry) == '\0') {
      break;
    }
    ptrCur = ptrCur->ptrNext;
  }

  return ptrCur;
}

INI *
_searchSection (
  IN INI                          *Head,
  IN UINT32                       Order,
  OUT CHAR8                       *Section
)
/*++

Routine Description:

  Search for the specific section in the ini table

--*/
{
  INI     *ptrCur;
  UINT32  CurOrder;

  CurOrder = 0;
  ptrCur = Head;
  while (ptrCur != NULL) {
    if (AsciiStriCmp (Section, ptrCur->ptrSection) == 0) {
      if (CurOrder == Order) {
        break;
      }
      ++ CurOrder;
    }

    ptrCur = _getNextSection (ptrCur);
  }

  return ptrCur;
}

VOID
_freeItem (
  IN INI                          *ptrItem
  )
/*++

Routine Description:

  Free an INI item

--*/
{
  if (ptrItem->ptrSection != NULL) {
    FreePool (ptrItem->ptrSection);
    ptrItem->ptrSection = NULL;
  }
  if (ptrItem->ptrEntry != NULL) {
    FreePool (ptrItem->ptrEntry);
    ptrItem->ptrEntry = NULL;
  }
  if (ptrItem->ptrValue != NULL) {
    FreePool (ptrItem->ptrValue);
    ptrItem->ptrValue = NULL;
  }
  FreePool (ptrItem);
}

VOID
_rmComment (
  IN COMMENTLINE                  **CmtHead,
  IN INI                          *ptrItem
)
{
  COMMENTLINE *ptrCmtCur;
  COMMENTLINE *ptrCmtPrev;
  COMMENTLINE *ptrCmtNext;

  ptrCmtCur = *CmtHead;
  ptrCmtPrev = NULL;
  while (ptrCmtCur != NULL) {
    if (ptrCmtCur->commentNo == ptrItem->commentNo) {
      ptrCmtNext = ptrCmtCur->ptrNext;
      FreePool (ptrCmtCur->ptrComment);
      FreePool (ptrCmtCur);
      if (ptrCmtPrev == NULL) {
        *CmtHead = ptrCmtNext;
      } else {
        ptrCmtPrev->ptrNext = ptrCmtNext;
      }
      ptrCmtCur = ptrCmtNext;
    } else {
      ptrCmtPrev = ptrCmtCur;
      ptrCmtCur = ptrCmtCur->ptrNext;
    }
  }
}

CHAR16 *
TplStrDuplicate (
  IN CHAR16             *String
  )
{
  EFI_STATUS  Status;
  CHAR16      *Buffer;

  if (String == NULL) {
    return NULL;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  (StrLen (String) + 1) * sizeof(CHAR16),
                  (VOID **)&Buffer
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  StrCpy (Buffer, String);

  return Buffer;
}

EFI_STATUS
EFIAPI
EfiIniGetString (
  IN EFI_INI_FILE_HANDLE                    This,
  IN CHAR16                                 *Section,
  IN CHAR16                                 *Entry,
  OUT CHAR16                                *String,
  IN OUT UINT32                             *maxLength
  )
/*++

Routine Description:

  One interface function of the EFI_INI_FILE to get a string.

Arguments:

  This            - the interface instance structure.
  Section         - Unicode section name.
  Entry           - Unicode entry name.
  String          - Unicode string to be returned.
  maxLength       - On input, the size in bytes of String. On output,
                    the size, in bytes, of the String needed to obtain
                    the value (if the String was not large enough).

Returns:

  EFI_SUCCESS           - get string successfully.
  EFI_NOT_FOUND         - Entry not found.
  EFI_BUFFER_TOO_SMALL  - the maxLength is too small for the result.
                          maxLength has been updated with the size needed
                          to complete the request.
  EFI_INVALID_PARAMETER - invalid parameters.

--*/
{
  CHAR8   ptrSection[MAX_STRING_LEN + 1];
  CHAR8   ptrEntry[MAX_STRING_LEN + 1];
  CHAR8   ptrString[MAX_STRING_LEN + 1];
  INI     *ptrCur;

  CHAR8   tmpSection[MAX_STRING_LEN + 1];
  CHAR8   tmpEntry[MAX_STRING_LEN + 1];

  EFI_INI_FILE_PRIVATE_DATA       *Private;

  //
  // check the parameter
  //
  if (Section == NULL || Entry == NULL || String == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  StrCpy (String, L"");

  if (StrLen (Section) > MAX_STRING_LEN || StrLen(Entry) > MAX_STRING_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrSection, Section, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrEntry, Entry, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  AsciiStrCpy (tmpSection, ptrSection);
  _alltrim (tmpSection);

  AsciiStrCpy (tmpEntry, ptrEntry);
  _alltrim (tmpEntry);

  if (AsciiStrLen (tmpSection) == 0 || AsciiStrLen (tmpEntry) == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Private = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (This);

  //
  // file content are in memory
  // so to find the field
  //

  ptrCur = Private->Head;

  while (ptrCur != NULL) {
    //
    // the same, so find the field
    //
    if ((AsciiStriCmp (tmpSection, ptrCur->ptrSection) == 0) &&
        (AsciiStriCmp (tmpEntry, ptrCur->ptrEntry)     == 0)) {
      if (AsciiStrLen (ptrCur->ptrValue) < *maxLength) {
        AsciiStrCpy (ptrString, ptrCur->ptrValue);
        TestProfile_mbstowcs (String, ptrString, AsciiStrLen(ptrString) + 1);
        return EFI_SUCCESS;
      } else {
        AsciiStrnCpy (ptrString, ptrCur->ptrValue, *maxLength - 1);
        ptrString[*maxLength-1] = '\0';
        TestProfile_mbstowcs (String, ptrString, AsciiStrLen(ptrString) + 1);
        *maxLength = (UINT32) (AsciiStrLen (ptrCur->ptrValue) + 1);
        return EFI_BUFFER_TOO_SMALL;
      }
    }

    ptrCur = ptrCur->ptrNext;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
EfiIniSetString (
  IN EFI_INI_FILE_HANDLE                    This,
  IN CHAR16                                 *Section,
  IN CHAR16                                 *Entry,
  IN CHAR16                                 *String
  )
/*++

Routine Description:

  One interface function of the EFI_INI_FILE to set a string.

Arguments:

  This        - the interface instance structure.
  Section     - Unicode section name.
  Entry       - Unicode entry name.
  String      - Unicode string to be set.

Returns:

  EFI_SUCCESS           - set string successfully.
  EFI_INVALID_PARAMETER - invalid parameters.
  EFI_OUT_OF_RESOURCES  - not enough memory.

--*/
{
  CHAR8   ptrSection[MAX_STRING_LEN + 1];
  CHAR8   ptrEntry[MAX_STRING_LEN + 1];
  CHAR8   ptrString[MAX_STRING_LEN + 1];

  INI     *ptrCur;
  INI     *ptrPrev;
  INI     *ptrNew;

  CHAR8   tmpSection[MAX_STRING_LEN + 1];
  CHAR8   tmpEntry[MAX_STRING_LEN + 1];
  CHAR8   tmpString[MAX_STRING_LEN + 1];
  CHAR8   *tmpPtr;

  EFI_INI_FILE_PRIVATE_DATA   *Private;

  Private = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (This);

  //
  // check the parameter
  //
  if (Section == NULL || Entry == NULL || String == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (StrLen (Section) > MAX_STRING_LEN ||
      StrLen (Entry)   > MAX_STRING_LEN ||
      StrLen (String)  > MAX_STRING_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrSection, Section, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrEntry, Entry, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  AsciiStrCpy (tmpSection, ptrSection);
  _alltrim (tmpSection);

  AsciiStrCpy (tmpEntry, ptrEntry);
  _alltrim (tmpEntry);

  if (AsciiStrLen (tmpSection) == 0 || AsciiStrLen (tmpEntry) == 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrString, String, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  AsciiStrCpy (tmpString, ptrString);
  _alltrim (tmpString);

  //
  // if both section and entry are found
  //
  ptrCur = Private->Head;
  ptrPrev = NULL;
  while (ptrCur != NULL) {
    if (AsciiStriCmp (tmpSection, ptrCur->ptrSection) == 0) {
      if (AsciiStriCmp (tmpEntry, ptrCur->ptrEntry) == 0) {
        if (AsciiStriCmp (tmpString, ptrCur->ptrValue) != 0) {
          tmpPtr = AsciiDuplicateString (tmpString);
          if (tmpPtr == NULL) {
            return EFI_OUT_OF_RESOURCES;
          }
          FreePool (ptrCur->ptrValue);
          ptrCur->ptrValue = tmpPtr;
          Private->Modified = TRUE;
        }
        return EFI_SUCCESS;
      }
      ptrPrev = ptrCur;
    }
    ptrCur = ptrCur->ptrNext;
  }

  //
  // if not, should add a new item
  //
  ptrNew = (INI *) AllocateZeroPool (sizeof(INI));
  if (ptrNew == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrSection = AsciiDuplicateString (tmpSection);
  if (ptrNew->ptrSection == NULL) {
    _freeItem (ptrNew);
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrEntry = AsciiDuplicateString (tmpEntry);
  if (ptrNew->ptrEntry == NULL) {
    _freeItem (ptrNew);
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrValue = AsciiDuplicateString (tmpString);
  if (ptrNew->ptrValue == NULL) {
    _freeItem (ptrNew) ;
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // 1. if only section is found
  //
  if (ptrPrev != NULL) {
    ptrNew->commentNo = 0;
    ptrNew->ptrNext   = ptrPrev->ptrNext;
    ptrPrev->ptrNext  = ptrNew;
    if (Private->Tail == ptrPrev) {
      Private->Tail = ptrNew;
    }
    Private->Modified = TRUE;
    return EFI_SUCCESS;
  }

  //
  // 2. if section is not found, should add a new section head
  //
  ptrCur = ptrNew;

  ptrNew = (INI *) AllocateZeroPool (sizeof(INI));
  if (ptrNew == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrSection = AsciiDuplicateString (tmpSection);
  if (ptrNew->ptrSection == NULL) {
    _freeItem (ptrNew);
    _freeItem (ptrCur);
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrEntry = AsciiDuplicateString ("");
  if (ptrNew->ptrEntry == NULL) {
    _freeItem (ptrNew);
    _freeItem (ptrCur);
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrValue = AsciiDuplicateString ("");
  if (ptrNew->ptrValue == NULL) {
    _freeItem (ptrNew);
    _freeItem (ptrCur);
    return EFI_OUT_OF_RESOURCES;
  }

  ptrCur->commentNo = 0;
  ptrCur->ptrNext   = NULL;
  ptrNew->commentNo = 0;
  ptrNew->ptrNext   = ptrCur;
  if (Private->Tail != NULL) {
    Private->Tail->ptrNext = ptrNew;
  } else {
    Private->Head = ptrNew;
  }
  Private->Tail = ptrCur;

  Private->Modified = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
EfiIniRmSection (
  IN EFI_INI_FILE_HANDLE                    This,
  IN CHAR16                                 *Section
  )
/*++

Routine Description:

  One interface function of the EFI_INI_FILE to remove sections.

Arguments:

  This        - the interface instance structure.
  Section     - Unicode section name.

Returns:

  EFI_SUCCESS           - remove section successfully.
  EFI_INVALID_PARAMETER - invalid parameters.
  EFI_NOT_FOUND         - section not found.

--*/
{
  INI                             *ptrCur;
  INI                             *ptrPrev;
  INI                             *ptrNext;
  EFI_INI_FILE_PRIVATE_DATA       *Private;
  CHAR8                           ptrSection[MAX_STRING_LEN + 1];
  CHAR8                           tmpSection[MAX_STRING_LEN + 1];

  Private = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (This);

  if (Section == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrSection, Section, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }
  AsciiStrCpy (tmpSection, ptrSection);
  _alltrim (tmpSection);

  ptrCur = Private->Head;
  ptrPrev = NULL;
  while (ptrCur != NULL) {
    if (AsciiStriCmp (tmpSection, ptrCur->ptrSection) == 0) {
      Private->Modified = TRUE;
      _rmComment (&(Private->CommentLineHead), ptrCur);
      ptrNext = ptrCur->ptrNext;
      _freeItem (ptrCur);
      if (ptrPrev == NULL) {
        Private->Head = ptrNext;
      } else {
        ptrPrev->ptrNext = ptrNext;
      }
      if (ptrNext == NULL) {
        Private->Tail = ptrPrev;
      }
      ptrCur = ptrNext;
    } else {
      ptrPrev = ptrCur;
      ptrCur = ptrCur->ptrNext;
    }
  }

  if (Private->Modified) {
    return EFI_SUCCESS;
  } else {
    return EFI_NOT_FOUND;
  }
}

EFI_STATUS
EFIAPI
EfiIniGetStringByOrder (
  IN EFI_INI_FILE_HANDLE                    This,
  IN UINT32                                 Order,
  IN CHAR16                                 *Section,
  IN CHAR16                                 *Entry,
  OUT CHAR16                                *String,
  IN OUT UINT32                             *maxLength
  )
/*++

Routine Description:

  One interface function of the EFI_INI_FILE to get a string by section order.

Arguments:

  This      - the interface instance structure.
  Order     - the section's order.
  Section   - Unicode section name.
  Entry     - Unicode entry name.
  String    - Unicode string to be returned.
  maxLength - on input, the size in bytes of String. On output,
              the size, in bytes, of the String needed to obtain
              the value (if the String was not large enough).

Returns:

  EFI_SUCCESS           - get string successfully.
  EFI_NOT_FOUND         - Entry not found.
  EFI_BUFFER_TOO_SMALL  - the maxLength is too small for the result.
                          maxLength has been updated with the size needed
                          to complete the request.
  EFI_INVALID_PARAMETER - invalid parameters.

--*/
{
  CHAR8 ptrSection[MAX_STRING_LEN + 1];
  CHAR8 ptrEntry[MAX_STRING_LEN + 1];
  CHAR8 ptrString[MAX_STRING_LEN + 1];
  INI   *ptrCur;

  CHAR8 tmpSection[MAX_STRING_LEN + 1];
  CHAR8 tmpEntry[MAX_STRING_LEN + 1];

  EFI_INI_FILE_PRIVATE_DATA       *Private;

  //
  // check the parameter
  //
  if (Section == NULL || Entry == NULL || String == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  StrCpy (String, L"");

  if (StrLen (Section) > MAX_STRING_LEN || StrLen (Entry) > MAX_STRING_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrSection, Section, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrEntry, Entry, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  AsciiStrCpy (tmpSection, ptrSection);
  _alltrim (tmpSection);

  AsciiStrCpy (tmpEntry, ptrEntry);
  _alltrim (tmpEntry);

  if (AsciiStrLen (tmpSection) == 0 || AsciiStrLen (tmpEntry) == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Private = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (This);

  //
  // file content are in memory
  // so to find the field
  //

  // find the section first
  ptrCur = _searchSection (Private->Head, Order, tmpSection);

  if (ptrCur != NULL) {
    ptrCur = ptrCur->ptrNext;
  }

  while (ptrCur != NULL) {
    if (*(ptrCur->ptrEntry) == '\0') {
      //
      // reached another section
      //
      break;
    }
    if ((AsciiStriCmp (tmpSection, ptrCur->ptrSection) == 0) &&
        (AsciiStriCmp (tmpEntry, ptrCur->ptrEntry)     == 0)) {
      if (AsciiStrLen (ptrCur->ptrValue) < *maxLength) {
        AsciiStrCpy (ptrString, ptrCur->ptrValue);
        TestProfile_mbstowcs (String, ptrString, AsciiStrLen(ptrString) + 1);
        return EFI_SUCCESS;
      } else {
        AsciiStrnCpy (ptrString, ptrCur->ptrValue, *maxLength - 1);
        ptrString[*maxLength-1] = '\0';
        TestProfile_mbstowcs (String, ptrString, AsciiStrLen(ptrString) + 1 );
        *maxLength = (UINT32) (AsciiStrLen (ptrCur->ptrValue) + 1);
        return EFI_BUFFER_TOO_SMALL;
      }
    }

    ptrCur = ptrCur->ptrNext;
  }  // endof while

  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
EfiIniSetStringByOrder (
  IN EFI_INI_FILE_HANDLE                    This,
  IN UINT32                                 Order,
  IN CHAR16                                 *Section,
  IN CHAR16                                 *Entry,
  IN CHAR16                                 *String
  )
/*++

Routine Description:

  One interface function of the EFI_INI_FILE to set a string by section order.

Arguments:

  This        - the interface instance structure.
  Order       - the section's order.
  Section     - Unicode section name.
  Entry       - Unicode entry name.
  String      - Unicode string to be set.

Returns:

  EFI_SUCCESS           - set string successfully.
  EFI_INVALID_PARAMETER - invalid parameters.
  EFI_OUT_OF_RESOURCES  - not enough memory.

--*/
{
  CHAR8 ptrSection[MAX_STRING_LEN + 1];
  CHAR8 ptrEntry[MAX_STRING_LEN + 1];
  CHAR8 ptrString[MAX_STRING_LEN + 1];

  INI   *ptrCur;
  INI   *ptrPrev;
  INI   *ptrNew;

  CHAR8 tmpSection[MAX_STRING_LEN + 1];
  CHAR8 tmpEntry[MAX_STRING_LEN + 1];
  CHAR8 tmpString[MAX_STRING_LEN + 1];
  CHAR8 *tmpPtr;

  EFI_INI_FILE_PRIVATE_DATA        *Private;

  Private = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (This);

  //
  // check the parameter
  //
  if (Section == NULL || Entry == NULL || String == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (StrLen (Section) > MAX_STRING_LEN ||
      StrLen (Entry)   > MAX_STRING_LEN ||
      StrLen (String)  > MAX_STRING_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrSection, Section, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrEntry, Entry, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  AsciiStrCpy (tmpSection, ptrSection);
  _alltrim (tmpSection);

  AsciiStrCpy (tmpEntry, ptrEntry);
  _alltrim (tmpEntry);

  if (AsciiStrLen (tmpSection) == 0 || AsciiStrLen (tmpEntry) == 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrString, String, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  AsciiStrCpy (tmpString, ptrString);
  _alltrim (tmpString);

  //
  // if both section and entry are found
  //
  ptrCur = _searchSection (Private->Head, Order, tmpSection);
  ptrPrev = ptrCur;
  if (ptrCur != NULL) {
    ptrCur = ptrCur->ptrNext;
  }
  while (ptrCur != NULL) {
    if (*(ptrCur->ptrEntry) == '\0') {
      //
      // reached another section
      //
      break;
    }
    if ((AsciiStriCmp (tmpSection, ptrCur->ptrSection ) == 0) &&
        (AsciiStriCmp (tmpEntry, ptrCur->ptrEntry )     == 0)) {
      if (AsciiStriCmp( tmpString, ptrCur->ptrValue) != 0) {
        tmpPtr = AsciiDuplicateString (tmpString);
        if (tmpPtr == NULL) {
          return EFI_OUT_OF_RESOURCES;
        }
        FreePool (ptrCur->ptrValue);
        ptrCur->ptrValue = tmpPtr;
        Private->Modified = TRUE;
      }
      return EFI_SUCCESS;
    }
    ptrPrev = ptrCur;
    ptrCur = ptrCur->ptrNext;
  }

  //
  // if not, should add a new item
  //
  ptrNew = (INI *) AllocateZeroPool (sizeof(INI));
  if (ptrNew == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrSection = AsciiDuplicateString (tmpSection);
  if (ptrNew->ptrSection == NULL) {
    _freeItem (ptrNew);
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrEntry = AsciiDuplicateString (tmpEntry);
  if (ptrNew->ptrEntry == NULL) {
    _freeItem (ptrNew);
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrValue = AsciiDuplicateString (tmpString);
  if (ptrNew->ptrValue == NULL) {
    _freeItem (ptrNew);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // 1. if only section is found
  //
  if (ptrPrev != NULL) {
    ptrNew->commentNo = 0;
    ptrNew->ptrNext = ptrPrev->ptrNext;
    ptrPrev->ptrNext = ptrNew;
    if (Private->Tail == ptrPrev) {
      Private->Tail = ptrNew;
    }
    Private->Modified = TRUE;
    return EFI_SUCCESS;
  }

  //
  // 2. if section is not found, should add a new section head
  //
  ptrCur = ptrNew;

  ptrNew = (INI *) AllocateZeroPool (sizeof(INI));
  if (ptrNew == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrSection = AsciiDuplicateString (tmpSection);
  if ( ptrNew->ptrSection == NULL ) {
    _freeItem (ptrNew);
    _freeItem (ptrCur);
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrEntry = AsciiDuplicateString ("");
  if ( ptrNew->ptrEntry == NULL ) {
    _freeItem (ptrNew);
    _freeItem (ptrCur);
    return EFI_OUT_OF_RESOURCES;
  }
  ptrNew->ptrValue = AsciiDuplicateString ("");
  if ( ptrNew->ptrValue == NULL ) {
    _freeItem (ptrNew);
    _freeItem (ptrCur);
    return EFI_OUT_OF_RESOURCES;
  }

  ptrCur->commentNo = 0;
  ptrCur->ptrNext   = NULL;
  ptrNew->commentNo = 0;
  ptrNew->ptrNext   = ptrCur;
  if (Private->Tail != NULL) {
    Private->Tail->ptrNext = ptrNew;
  } else {
    Private->Head = ptrNew;
  }
  Private->Tail = ptrCur;

  Private->Modified = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
EfiIniRmSectionByOrder (
  IN EFI_INI_FILE_HANDLE                    This,
  IN UINT32                                 Order,
  IN CHAR16                                 *Section
  )
/*++

Routine Description:

  One interface function of the EFI_INI_FILE to remove a section by order.

Arguments:

  This        - the interface instance structure.
  Order       - the section's order.
  Section     - Unicode section name.

Returns:

  EFI_SUCCESS           - remove section successfully.
  EFI_INVALID_PARAMETER - invalid parameters.
  EFI_NOT_FOUND         - section not found.

--*/
{
  INI                              *ptrCur;
  INI                              *ptrPrev;
  INI                              *ptrSect;
  INI                              *ptrNext;
  EFI_INI_FILE_PRIVATE_DATA        *Private;
  CHAR8                            ptrSection[MAX_STRING_LEN + 1];
  CHAR8                            tmpSection[MAX_STRING_LEN + 1];

  Private = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (This);

  if (Section == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrSection, Section, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }
  AsciiStrCpy (tmpSection, ptrSection);
  _alltrim (tmpSection);

  ptrSect = _searchSection (Private->Head, Order, tmpSection);
  if (ptrSect == NULL) {
    return EFI_NOT_FOUND;
  }

  ptrCur = Private->Head;
  ptrPrev = NULL;
  while (ptrCur != NULL) {
    if (ptrCur == ptrSect) {
      break;
    }
    ptrPrev = ptrCur;
    ptrCur = ptrCur->ptrNext;
  }

  _rmComment (&(Private->CommentLineHead), ptrSect);
  ptrNext = ptrSect->ptrNext;
  _freeItem (ptrSect);
  ptrCur = ptrNext;
  while (ptrCur != NULL) {
    if (*(ptrCur->ptrEntry) == '\0') {
      //
      // reached another section
      //
      break;
    }
    _rmComment (&(Private->CommentLineHead), ptrCur);
    ptrNext = ptrCur->ptrNext;
    _freeItem (ptrCur);
    ptrCur = ptrNext;
  }

  if (ptrPrev == NULL) {
    Private->Head = ptrNext;
  } else {
    ptrPrev->ptrNext = ptrNext;
  }

  if (ptrNext == NULL) {
    Private->Tail = ptrPrev;
  }

  Private->Modified = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
EfiIniGetOrderNum (
  IN EFI_INI_FILE_HANDLE                    This,
  IN CHAR16                                 *Section,
  OUT UINT32                                *OrderNum
  )
/*++

Routine Description:

  One interface function of the EFI_INI_FILE to get the number of sections with the same name.

Arguments:

  This        - the interface instance structure.
  Section     - Unicode section name.
  OrderNum    - return the number of sections with the same name.

Returns:

  EFI_SUCCESS           - get number successfully.
  EFI_INVALID_PARAMETER - invalid parameters.

--*/
{
  INI                              *ptrCur;
  EFI_INI_FILE_PRIVATE_DATA        *Private;
  CHAR8                            ptrSection[MAX_STRING_LEN + 1];
  CHAR8                            tmpSection[MAX_STRING_LEN + 1];

  Private = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (This);

  if ((OrderNum == NULL) || (Section == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (TestProfile_wcstombs (ptrSection, Section, MAX_STRING_LEN + 1) == -1) {
    return EFI_INVALID_PARAMETER;
  }

  AsciiStrCpy (tmpSection, ptrSection);
  _alltrim (tmpSection);

  *OrderNum = 0;
  ptrCur = Private->Head;
  while (ptrCur != NULL) {
    if (AsciiStriCmp (tmpSection, ptrCur->ptrSection) == 0) {
      ++ (*OrderNum);
    }

    ptrCur = _getNextSection (ptrCur);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
EfiIniFlush (
  IN EFI_INI_FILE_HANDLE                    This
  )
/*++

Routine Description:

  One interface function of the EFI_INI_FILE to flush an ini file.

Arguments:

  This        - the interface instance structure.

Returns:

  EFI_SUCCESS - the ini file was written to the disk successfully.

--*/
{
  INI                     *ptrCur;
  COMMENTLINE             *ptrCmtCur;
  CHAR8                   ptrCurSection[MAX_STRING_LEN + 1];
  BOOLEAN                 first;
  UINT32                  commentNo;

  EFI_STATUS              Status;
  EFI_HANDLE              DeviceHandle;
  EFI_FILE_HANDLE         RootDir;
  EFI_FILE_HANDLE         Handle;
  UINTN                   BufSize;
  CHAR8                   Buffer[MAX_LINE_LEN];
  CHAR8                   Line[MAX_LINE_LEN * 2];

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Vol;
  EFI_DEVICE_PATH_PROTOCOL         *RootDevPath;
  EFI_INI_FILE_PRIVATE_DATA        *Private;

  Private = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (This);

  commentNo = 0;

  //
  // File not modified, needn't to write back to the disk
  //
  if (!Private->Modified) {
    return EFI_SUCCESS;
  }

  RootDevPath = Private->DevPath;
  //
  //  Determine device handle for fs protocol on specified device path
  //
  Status = gBS->LocateDevicePath (
                  &gEfiSimpleFileSystemProtocolGuid,
                  &RootDevPath,
                  &DeviceHandle
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  //  Determine volume for file system on device handle
  //
  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID*)&Vol
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Open volume for file system on device path
  //
  Status = Vol->OpenVolume (Vol, &RootDir);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // open and delete the file before write
  //
  Status = RootDir->Open (
                      RootDir,
                      &Handle,
                      Private->FileName,
                      EFI_FILE_MODE_CREATE|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_READ,
                      0
                      );
  if (EFI_ERROR (Status)) {
    RootDir->Close (RootDir);
    return Status;
  }

  Status = Handle->Delete (Handle);
  //
  // EFI_FILE.Delete() return a warning status
  //
  if (Status != EFI_SUCCESS) {
    RootDir->Close (RootDir);
    return EFI_UNSUPPORTED;
  }

  //
  // reopen the file to write
  //
  Status = RootDir->Open (
                      RootDir,
                      &Handle,
                      Private->FileName,
                      EFI_FILE_MODE_CREATE|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_READ,
                      0
                      );
  if (EFI_ERROR (Status)) {
    RootDir->Close (RootDir);
    return Status;
  }

  ptrCur = Private->Head ;
  AsciiStrCpy (ptrCurSection, "");
  first = TRUE;

  //
  // write file head
  //
  if (Private->isUnicode) {
    Buffer[0] = 0xff;
    Buffer[1] = 0xfe;
    BufSize = 2;
    Status = Handle->Write (Handle, &BufSize, Buffer);
    if (EFI_ERROR (Status)) {
      Handle->Close (Handle);
      RootDir->Close (RootDir);
      return Status;
    }
  }

  //
  // write file line by line
  //
  while (ptrCur != NULL) {
    if (*(ptrCur->ptrEntry) == '\0') {
      //
      // New section encountered
      // If it's not the first section, add a blank line
      //
      if (first) {
        first = FALSE;
      } else {
        AsciiStrCpy (Buffer, "\r\n");
        BufSize = AsciiStrLen (Buffer);
        if (Private->isUnicode) {
          TestProfile_mbstowcs ((CHAR16 *)Line, Buffer, BufSize + 1);
          BufSize *= 2;
          Status = Handle->Write (Handle, &BufSize, Line);
        } else {
          Status = Handle->Write (Handle, &BufSize, Buffer);
        }
        if (EFI_ERROR (Status)) {
          Handle->Close (Handle);
          RootDir->Close (RootDir);
          return Status;
        }
      }
    }

    //
    //  If there are comment lines before this line, write comment lines first
    //
    ptrCmtCur = Private->CommentLineHead;
    while (ptrCmtCur != NULL) {
      if (ptrCmtCur->commentNo == ptrCur->commentNo) {
        commentNo = ptrCmtCur->commentNo;
        AsciiStrCpy (Buffer, ptrCmtCur->ptrComment);
        AsciiStrCat (Buffer, "\r\n");
        BufSize = AsciiStrLen (Buffer);
        if (Private->isUnicode) {
          TestProfile_mbstowcs ((CHAR16 *)Line, Buffer, BufSize + 1);
          BufSize *= 2;
          Status = Handle->Write (Handle, &BufSize, Line);
        } else {
          Status = Handle->Write (Handle, &BufSize, Buffer);
        }
        if (EFI_ERROR (Status)) {
          Handle->Close (Handle);
          RootDir->Close (RootDir);
          return Status;
        }
      }
      ptrCmtCur = ptrCmtCur->ptrNext ;
    }

    if (*(ptrCur->ptrEntry) == '\0') {
      //
      // new section, write the section head
      //
      AsciiStrCpy (ptrCurSection, ptrCur->ptrSection);
      AsciiStrCpy (Buffer, "[");
      AsciiStrCat (Buffer, ptrCurSection);
      AsciiStrCat (Buffer, "]\r\n");
      BufSize = AsciiStrLen (Buffer);
      if (Private->isUnicode) {
        TestProfile_mbstowcs ((CHAR16 *)Line, Buffer, BufSize + 1);
        BufSize *= 2;
        Status = Handle->Write (Handle, &BufSize, Line);
      } else {
        Status = Handle->Write (Handle, &BufSize, Buffer);
      }
      if (EFI_ERROR (Status)) {
        Handle->Close (Handle);
        RootDir->Close (RootDir);
        return Status;
      }
    } else {
      //
      // write the entry and value line
      //
      AsciiStrCpy (Buffer, ptrCur->ptrEntry);
      AsciiStrCat (Buffer, "=");
      AsciiStrCat (Buffer, ptrCur->ptrValue);
      AsciiStrCat (Buffer, "\r\n");
      BufSize = AsciiStrLen (Buffer);
      if (Private->isUnicode) {
        TestProfile_mbstowcs ((CHAR16 *)Line, Buffer, BufSize + 1);
        BufSize *= 2;
        Status = Handle->Write (Handle, &BufSize, Line);
      } else {
        Status = Handle->Write (Handle, &BufSize, Buffer);
      }
      if (EFI_ERROR (Status)) {
        Handle->Close (Handle);
        RootDir->Close (RootDir);
        return Status;
      }
    }

    ptrCur = ptrCur->ptrNext;
  }

  //
  //  If there are comment lines in the end of the file, write these comment lines
  //
  ptrCmtCur = Private->CommentLineHead;
  commentNo ++;
  while (ptrCmtCur != NULL) {
    if (ptrCmtCur->commentNo >= commentNo) {
      AsciiStrCpy (Buffer, ptrCmtCur->ptrComment);
      AsciiStrCat (Buffer, "\r\n");
      BufSize = AsciiStrLen (Buffer);
      if (Private->isUnicode) {
        TestProfile_mbstowcs ((CHAR16 *)Line, Buffer, BufSize + 1);
        BufSize *= 2;
        Status = Handle->Write (Handle, &BufSize, Line);
      } else {
        Status = Handle->Write (Handle, &BufSize, Buffer);
      }
      if (EFI_ERROR (Status)) {
        Handle->Close (Handle);
        RootDir->Close (RootDir);
        return Status;
      }
    }
    ptrCmtCur = ptrCmtCur->ptrNext;
  }

  Handle->Flush (Handle);
  Handle->Close (Handle);
  RootDir->Close (RootDir);
  Private->Modified = FALSE;
  return EFI_SUCCESS;
}


VOID
_initFile (
  IN EFI_INI_FILE_PRIVATE_DATA    *IniFile
  )
/*++

Routine Description:

  Initialize a new ini file.

--*/
{
  IniFile->Signature                 = EFI_INI_FILE_PRIVATE_DATA_SIGNATURE;
  IniFile->Handle.Revision           = 0x10000;
  IniFile->Handle.GetString          = EfiIniGetString;
  IniFile->Handle.SetString          = EfiIniSetString;
  IniFile->Handle.RmSection          = EfiIniRmSection;
  IniFile->Handle.GetStringByOrder   = EfiIniGetStringByOrder;
  IniFile->Handle.SetStringByOrder   = EfiIniSetStringByOrder;
  IniFile->Handle.RmSectionByOrder   = EfiIniRmSectionByOrder;
  IniFile->Handle.GetOrderNum        = EfiIniGetOrderNum;
  IniFile->Handle.Flush              = EfiIniFlush;
  IniFile->DevPath                   = NULL;
  IniFile->FileName                  = NULL;
  IniFile->Head                      = NULL;
  IniFile->Tail                      = NULL;
  IniFile->CommentLineHead           = NULL;
  IniFile->isUnicode                 = FALSE;
  IniFile->Modified                  = FALSE;
}

VOID
_freeIniFile (
  IN EFI_INI_FILE_PRIVATE_DATA    *IniFile
  )
/*++

Routine Description:

  Free an ini file and all the resources related to the ini file.

--*/
{
  IniFile->Handle.GetString          = NULL;
  IniFile->Handle.SetString          = NULL;
  IniFile->Handle.RmSection          = NULL;
  IniFile->Handle.GetStringByOrder   = NULL;
  IniFile->Handle.SetStringByOrder   = NULL;
  IniFile->Handle.RmSectionByOrder   = NULL;
  IniFile->Handle.GetOrderNum        = NULL;
  IniFile->Handle.Flush              = NULL;
  if (IniFile->FileName != NULL) {
    FreePool (IniFile->FileName);
    IniFile->FileName = NULL;
  }
  if (IniFile->DevPath != NULL) {
    FreePool (IniFile->DevPath);
    IniFile->DevPath = NULL;
  }
  _freePointer(IniFile);
  FreePool (IniFile);
}

EFI_STATUS
EFIAPI
AtslEfiIniOpen (
  IN EFI_TEST_PROFILE_LIBRARY_PROTOCOL     *This,
  IN EFI_DEVICE_PATH_PROTOCOL               *RootDevPath,
  IN CHAR16                                 *FileName,
  OUT EFI_INI_FILE_HANDLE                   *FileHandle
  )
/*++

Routine Description:

  One interface function of the TestProfileLibrary to open an ini file.

Arguments:

  This                  - the protocol instance structure.
  RootDevPath           - root device path of the ini file.
  FileName              - Unicode file name of the ini file relative to the
                          RootDevPath.
  FileHandle            - return the opened handle of the ini file.

Returns:

  EFI_SUCCESS           - open the ini file successfully.
  EFI_INVALID_PARAMETER - invalid parameters.
  EFI_OUT_OF_RESOURCES  - not enough memory.

--*/
{
  EFI_STATUS                        Status;
  EFI_INI_FILE_PRIVATE_DATA         *NewIniFile;
  EFI_HANDLE                        DeviceHandle;
  EFI_FILE_HANDLE                   RootDir;
  EFI_FILE_HANDLE                   Handle;
  UINTN                             Index;
  UINTN                             Number;
  UINTN                             BufSize;
  CHAR8                             ptrLine[MAX_LINE_LEN] ;
  CHAR8                             Buffer[MAX_LINE_LEN * 2];
  CHAR8                             ptrSection[MAX_STRING_LEN + 1] ;
  UINT32                            commentNo ;
  BOOLEAN                           isSectionGot ;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *Vol;

  //
  // Check the parameters
  //
  if ((RootDevPath == NULL) || (FileName == NULL) || (FileHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (StrLen (FileName) > MAX_STRING_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // New and initialize a ini file
  //
  NewIniFile = AllocatePool (sizeof(EFI_INI_FILE_PRIVATE_DATA));
  if (NewIniFile == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  _initFile (NewIniFile);

  NewIniFile->FileName = UnicodeDuplicateString (FileName);
  if ( NewIniFile->FileName == NULL ) {
    _freeIniFile (NewIniFile);
    return EFI_OUT_OF_RESOURCES;
  }

  NewIniFile->DevPath = DuplicateDevicePath (RootDevPath);
  if ( NewIniFile->DevPath == NULL ) {
    _freeIniFile (NewIniFile);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // current section name is null
  //
  ptrSection[0] = '\0';

  //
  // section not found
  //
  isSectionGot = FALSE;

  //
  // current comment number :
  // every comment line's comment number is the same as the comment number of
  // the first non comment line after this comment line.
  //
  commentNo = 1;

  //
  //  Determine device handle for fs protocol on specified device path
  //
  Status = gBS->LocateDevicePath (
                  &gEfiSimpleFileSystemProtocolGuid,
                  &RootDevPath,
                  &DeviceHandle
                  );
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    return Status;
  }

  //
  //  Determine volume for file system on device handle
  //
  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID*)&Vol
                  );
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    return Status;
  }

  //
  // Open volume for file system on device path
  //
  Status = Vol->OpenVolume (Vol, &RootDir);
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    return Status;
  }

  //
  // Open profile for read
  //
  Status = RootDir->Open (
                      RootDir,
                      &Handle,
                      FileName,
                      EFI_FILE_MODE_READ,
                      0
                      );
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    RootDir->Close (RootDir);
    return Status;
  }

  //
  // Determine the file type: unicode or ansi
  //
  BufSize = MAX_LINE_LEN * 2;
  Status = Handle->Read (Handle, &BufSize, Buffer);
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    Handle->Close (Handle);
    RootDir->Close (RootDir);
    return Status;
  }

  if ((BufSize >= 2) && (Buffer[0] == 0xff) && (Buffer[1] == 0xfe)) {
    NewIniFile->isUnicode = TRUE;
    Number = 2;
  } else {
    Number = 0;
  }
  Index = 0;

  //
  // process the profile line by line
  //
  while (BufSize != 0) {
    while (Number < BufSize) {
      ptrLine[Index++] = Buffer[Number];
      if ((Buffer[Number] == '\n') || (Index >= MAX_LINE_LEN - 1)) {
        //
        // a line is read, process it
        //
        ptrLine[Index] = '\0';
        Index = 0;
        _prosessLine (
          NewIniFile,
          ptrLine,
          ptrSection,
          &isSectionGot,
          &commentNo
        );
      }

      if (NewIniFile->isUnicode) {
        Number += 2;
      } else {
        Number ++;
      }
    }

    Number = 0;
    BufSize = MAX_LINE_LEN * 2;
    Status = Handle->Read (Handle, &BufSize, Buffer);
    if (EFI_ERROR (Status)) {
      _freeIniFile (NewIniFile);
      Handle->Close (Handle);
      RootDir->Close (RootDir);
      return Status;
    }
  }

  //
  // process the last line without '\n'
  //
  ptrLine[Index] = '\0';
  Index = 0;
  _prosessLine (
    NewIniFile,
    ptrLine,
    ptrSection,
    &isSectionGot,
    &commentNo
  );

  //
  // close the profile, and return file handle
  //
  Handle->Close (Handle);
  RootDir->Close (RootDir);
  *FileHandle = &(NewIniFile->Handle);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AtslEfiIniCreate (
  IN EFI_TEST_PROFILE_LIBRARY_PROTOCOL     *This,
  IN EFI_DEVICE_PATH_PROTOCOL               *RootDevPath,
  IN CHAR16                                 *FileName,
  OUT EFI_INI_FILE_HANDLE                   *FileHandle
  )
/*++

Routine Description:

  One interface function of the TestProfileLibrary to create an ini file.

Arguments:

  This                  - the protocol instance structure.
  RootDevPath           - root device path of the ini file.
  FileName              - Unicode file name of the ini file relative to the
                          RootDevPath.
  FileHandle            - return the created handle of the ini file.

Returns:

  EFI_SUCCESS           - create the ini file successfully.
  EFI_INVALID_PARAMETER - invalid parameters.
  EFI_OUT_OF_RESOURCES  - not enough memory.

--*/
{
  EFI_STATUS                        Status;
  EFI_INI_FILE_PRIVATE_DATA         *NewIniFile;
  EFI_HANDLE                        DeviceHandle;
  EFI_FILE_HANDLE                   RootDir;
  EFI_FILE_HANDLE                   Handle;
  UINTN                             BufSize;
  CHAR8                             Buffer[2];
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *Vol;

  //
  // Check the parameters
  //
  if ((RootDevPath == NULL) || (FileName == NULL) || (FileHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (StrLen (FileName) > MAX_STRING_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // New and initialize a ini file
  //
  NewIniFile = AllocatePool (sizeof(EFI_INI_FILE_PRIVATE_DATA));
  if (NewIniFile == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  _initFile (NewIniFile);

  NewIniFile->FileName = UnicodeDuplicateString (FileName);
  if ( NewIniFile->FileName == NULL ) {
    _freeIniFile (NewIniFile);
    return EFI_OUT_OF_RESOURCES;
  }

  NewIniFile->DevPath = DuplicateDevicePath (RootDevPath);
  if ( NewIniFile->DevPath == NULL ) {
    _freeIniFile (NewIniFile);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  //  Determine device handle for fs protocol on specified device path
  //
  Status = gBS->LocateDevicePath (
                  &gEfiSimpleFileSystemProtocolGuid,
                  &RootDevPath,
                  &DeviceHandle
                  );
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    return Status;
  }

  //
  //  Determine volume for file system on device handle
  //
  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID*)&Vol
                  );
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    return Status;
  }

  //
  // Open volume for file system on device path
  //
  Status = Vol->OpenVolume (Vol, &RootDir);
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    return Status;
  }

  //
  // Open profile to test the existence
  //
  Status = RootDir->Open (
                      RootDir,
                      &Handle,
                      FileName,
                      EFI_FILE_MODE_READ,
                      0
                      );
  if (Status != EFI_NOT_FOUND) {
    _freeIniFile (NewIniFile);
    RootDir->Close (RootDir);
    return EFI_ACCESS_DENIED;
  }

  //
  // New profile
  //
  Status = RootDir->Open (
                      RootDir,
                      &Handle,
                      FileName,
                      EFI_FILE_MODE_CREATE|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_READ,
                      0
                      );
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    RootDir->Close (RootDir);
    return Status;
  }

  NewIniFile->isUnicode = TRUE;
  Buffer[0] = 0xff;
  Buffer[1] = 0xfe;
  BufSize = 2;
  Status = Handle->Write (Handle, &BufSize, Buffer);
  if (EFI_ERROR (Status)) {
    _freeIniFile (NewIniFile);
    Handle->Close (Handle);
    RootDir->Close (RootDir);
    return Status;
  }

  //
  // close the profile, and return file handle
  //
  Handle->Close (Handle);
  RootDir->Close (RootDir);
  *FileHandle = &(NewIniFile->Handle);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AtslEfiIniClose (
  IN EFI_TEST_PROFILE_LIBRARY_PROTOCOL     *This,
  IN EFI_INI_FILE_HANDLE                    FileHandle
  )
/*++

Routine Description:

  One interface function of the TestProfileLibrary to close an ini file.

Arguments:

  This        - the protocol instance structure.
  FileHandle  - ini file to be closed.

Returns:

  EFI_SUCCESS - the ini file was closed successfully.

--*/
{
  EFI_STATUS                      Status;
  EFI_INI_FILE_PRIVATE_DATA       *IniFile;

  IniFile = EFI_INI_FILE_PRIVATE_DATA_FROM_THIS (FileHandle);

  Status = FileHandle->Flush (FileHandle);
  _freeIniFile (IniFile);
  return Status;
}

EFI_STATUS
EFIAPI
AtslEfiGetSystemDevicePath (
  IN EFI_TEST_PROFILE_LIBRARY_PROTOCOL     *This,
  OUT EFI_DEVICE_PATH_PROTOCOL              **DevicePath,
  OUT CHAR16                                **FilePath
  )
/*++

Routine Description:

  One interface function of the TestProfileLibrary to get system device path and
  file path.

Arguments:

  This        - the protocol instance structure.
  DevicePath  - the device path of this framework.
  FilePath    - the file path of this framework.

Returns:

  EFI_SUCCESS - get the device path and file path successfully.

--*/
{
  TEST_PROFILE_PRIVATE_DATA      *Private;

  Private = TEST_PROFILE_PRIVATE_DATA_FROM_THIS (This);

  if ((DevicePath == NULL) || (FilePath == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get system device path
  //
  *DevicePath = DuplicateDevicePath (Private->DevicePath);
  if (*DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Get system file path
  //
  *FilePath = TplStrDuplicate (Private->FilePath);
  if (*FilePath == NULL) {
    gBS->FreePool (*DevicePath);
    *DevicePath = NULL;
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AtslEfiSetSystemDevicePath (
  IN EFI_TEST_PROFILE_LIBRARY_PROTOCOL     *This,
  IN EFI_DEVICE_PATH_PROTOCOL               *DevicePath,
  IN CHAR16                                 *FilePath
  )
/*++

Routine Description:

  One interface function of the TestProfileLibrary to set system device path and
  file path.

Arguments:

  This        - the protocol instance structure.
  DevicePath  - the device path of this framework.
  FilePath    - the file path of this framework.

Returns:

  EFI_SUCCESS - set the device path and file path successfully.

--*/
{
  TEST_PROFILE_PRIVATE_DATA      *Private;

  Private = TEST_PROFILE_PRIVATE_DATA_FROM_THIS (This);

  if ((DevicePath == NULL) || (FilePath == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Set system device path
  //
  Private->DevicePath = DevicePath;

  //
  // Set system file path
  //
  Private->FilePath = FilePath;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
TslOpen (
  IN EFI_TSL_INIT_INTERFACE       *This,
  IN OUT EFI_HANDLE               *LibHandle,
  OUT VOID                        **PrivateLibInterface
  )
/*++

Routine Description:

  Opens the test support library to get the handle with the public interface
  and the private interface.

Arguments:

  This              - TSL Initiation Interface instance.
  LibHandle         - The pointer to the handle on which the public interface
                      is installed.
  PrivateInterface  - The private interface of the test support library.

Returns:

  EFI_SUCCESS           - open the StandardTestLibrary successfully.
  EFI_INVALID_PARAMETER - invalid parameter, LibHandle is NULL.
  EFI_ALREADY_STARTED   - the StandardTestLibrary has been bind on the LibHandle
                          before.

--*/
{
  EFI_STATUS                  Status;
  TEST_PROFILE_PRIVATE_DATA  *Private;
  TSL_INIT_PRIVATE_DATA       *TslPrivate;

  //
  // Check parameter
  //
  if (LibHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  TslPrivate = TSL_INIT_PRIVATE_DATA_FROM_THIS (This);

  //
  // Open the TestProfileLibrary protocol to perform the supported test.
  //
  if (*LibHandle != NULL) {
    Status = gBS->OpenProtocol (
                    *LibHandle,
                    &gEfiTestProfileLibraryGuid,
                    NULL,
                    TslPrivate->ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_TEST_PROTOCOL
                    );
    if (!EFI_ERROR (Status)) {
      return EFI_ALREADY_STARTED;
    }
  }

  //
  // Initialize the TestProfileLibrary private data
  //
  Status = gBS->AllocatePool(
                  EfiBootServicesData,
                  sizeof (TEST_PROFILE_PRIVATE_DATA),
                  (VOID **)&Private
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (Private, sizeof(TEST_PROFILE_PRIVATE_DATA));
  Private->Signature                        = TEST_PROFILE_PRIVATE_DATA_SIGNATURE;
  Private->TestProfile.LibraryRevision     = 0x10000;
  Private->TestProfile.Name                = gAtslName;
  Private->TestProfile.Description         = gAtslDescription;
  Private->TestProfile.EfiIniOpen          = AtslEfiIniOpen;
  Private->TestProfile.EfiIniCreate        = AtslEfiIniCreate;
  Private->TestProfile.EfiIniClose         = AtslEfiIniClose;
  Private->TestProfile.EfiGetSystemDevicePath    = AtslEfiGetSystemDevicePath;
  Private->TestProfile.EfiSetSystemDevicePath    = AtslEfiSetSystemDevicePath;

  if (PrivateLibInterface != NULL) {
    //
    // currently there is no private interface
    //
    *PrivateLibInterface = NULL;
  }

  //
  // Install TestProfileLibrary protocol
  //
  Status = gBS->InstallProtocolInterface (
                  LibHandle,
                  &gEfiTestProfileLibraryGuid,
                  EFI_NATIVE_INTERFACE,
                  &(Private->TestProfile)
                  );

  return Status;
}

EFI_STATUS
EFIAPI
TslClose (
  IN EFI_TSL_INIT_INTERFACE       *This,
  IN EFI_HANDLE                   LibHandle
  )
/*++

Routine Description:

  Closes the test support library to free the public interface.

Arguments:

  This              - TSL Initiation Interface instance.
  LibHandle         - The handle on which the public interface was installed.

Returns:

  EFI_SUCCESS   - close the TestProfileLibrary successfully.

--*/
{
  EFI_STATUS                          Status;
  TEST_PROFILE_PRIVATE_DATA          *Private;
  TSL_INIT_PRIVATE_DATA               *TslPrivate;
  EFI_TEST_PROFILE_LIBRARY_PROTOCOL  *TestProfile;

  TslPrivate = TSL_INIT_PRIVATE_DATA_FROM_THIS (This);

  //
  // Open the TestProfileLibrary protocol to perform the supported test.
  //
  Status = gBS->OpenProtocol (
                  LibHandle,
                  &gEfiTestProfileLibraryGuid,
                  (VOID **)&TestProfile,
                  TslPrivate->ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Uninstall TestProfileLibrary protocol
  //
  Status = gBS->UninstallProtocolInterface (
                  LibHandle,
                  &gEfiTestProfileLibraryGuid,
                  TestProfile
                  );
  Private = TEST_PROFILE_PRIVATE_DATA_FROM_THIS (TestProfile);
  gBS->FreePool (Private);

  return Status;
}

EFI_STATUS
EFIAPI
TslInitUnload (
  IN EFI_HANDLE         ImageHandle
  )
/*++

Routine Description:

  Unload function for the driver, uninstall TslInit protocol.

Arguments:

  ImageHandle   - the driver image handle.

Returns:

  EFI_SUCCESS   - unload successfully.

--*/
{
  EFI_STATUS              Status;
  EFI_TSL_INIT_INTERFACE  *TslInit;
  TSL_INIT_PRIVATE_DATA   *Private;

  //
  // Open the TslInit protocol
  //
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiTslInitInterfaceGuid,
                  (VOID **)&TslInit,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (!EFI_ERROR (Status)) {
    //
    // Uninstall TslInit protocol
    //
    Status = gBS->UninstallProtocolInterface (
                    ImageHandle,
                    &gEfiTslInitInterfaceGuid,
                    TslInit
                    );

    Private = TSL_INIT_PRIVATE_DATA_FROM_THIS (TslInit);
    gBS->FreePool (Private);
  }

  return Status;
}

//
// Entry point
//
EFI_STATUS
EFIAPI
TestProfileEntryPoint (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
/*++

Routine Description:

  Test profile library driver's entry point.

Arguments:

  ImageHandle         - the driver image handle.
  SystemTable         - the system table.

Returns:

  EFI_SUCCESS         - the driver is loaded successfully.
  EFI_ALREADY_STARTED - the driver has already been loaded before.

--*/
{
  EFI_STATUS                  Status;
  EFI_LOADED_IMAGE_PROTOCOL   *LoadedImage;
  TSL_INIT_PRIVATE_DATA       *Private;

  //
  // Fill in the Unload() function
  //
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  LoadedImage->Unload = TslInitUnload;

  //
  // Open the TslInit protocol to perform the supported test.
  //
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiTslInitInterfaceGuid,
                  NULL,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_TEST_PROTOCOL
                  );
  if (!EFI_ERROR (Status)) {
    return EFI_ALREADY_STARTED;
  }

  //
  // Initialize the TslInit private data
  //
  Status = gBS->AllocatePool(
                  EfiBootServicesData,
                  sizeof (TSL_INIT_PRIVATE_DATA),
                  (VOID **)&Private
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (Private, sizeof(TSL_INIT_PRIVATE_DATA));
  Private->Signature            = TSL_INIT_PRIVATE_DATA_SIGNATURE;
  Private->ImageHandle          = ImageHandle;
  Private->TslInit.Revision     = 0x10000;
  Private->TslInit.LibraryGuid  = gEfiTestProfileLibraryGuid;
  Private->TslInit.Open         = TslOpen;
  Private->TslInit.Close        = TslClose;

  //
  // Install TslInit protocol
  //
  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEfiTslInitInterfaceGuid,
                  EFI_NATIVE_INTERFACE,
                  &(Private->TslInit)
                  );

  return Status;
}
