//===--- DeclCXX.cpp - C++ Declaration AST Node Implementation ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the C++ related Decl classes.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/DeclCXX.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTMutationListener.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/IdentifierTable.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
using namespace clang;

//===----------------------------------------------------------------------===//
// Decl Allocation/Deallocation Method Implementations
//===----------------------------------------------------------------------===//

void AccessSpecDecl::anchor() { }

AccessSpecDecl *AccessSpecDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(AccessSpecDecl));
  return new (Mem) AccessSpecDecl(EmptyShell());
}

CXXRecordDecl::DefinitionData::DefinitionData(CXXRecordDecl *D)
  : UserDeclaredConstructor(false), UserDeclaredSpecialMembers(0),
    Aggregate(true), PlainOldData(true), Empty(true), Polymorphic(false),
    Abstract(false), IsStandardLayout(true), HasNoNonEmptyBases(true),
    HasPrivateFields(false), HasProtectedFields(false), HasPublicFields(false),
    HasMutableFields(false), HasOnlyCMembers(true),
    HasInClassInitializer(false), HasUninitializedReferenceMember(false),
    NeedOverloadResolutionForMoveConstructor(false),
    NeedOverloadResolutionForMoveAssignment(false),
    NeedOverloadResolutionForDestructor(false),
    DefaultedMoveConstructorIsDeleted(false),
    DefaultedMoveAssignmentIsDeleted(false),
    DefaultedDestructorIsDeleted(false),
    HasTrivialSpecialMembers(SMF_All),
    DeclaredNonTrivialSpecialMembers(0),
    HasIrrelevantDestructor(true),
    HasConstexprNonCopyMoveConstructor(false),
    DefaultedDefaultConstructorIsConstexpr(true),
    HasConstexprDefaultConstructor(false),
    HasNonLiteralTypeFieldsOrBases(false), ComputedVisibleConversions(false),
    UserProvidedDefaultConstructor(false), DeclaredSpecialMembers(0),
    ImplicitCopyConstructorHasConstParam(true),
    ImplicitCopyAssignmentHasConstParam(true),
    HasDeclaredCopyConstructorWithConstParam(false),
    HasDeclaredCopyAssignmentWithConstParam(false),
    FailedImplicitMoveConstructor(false), FailedImplicitMoveAssignment(false),
    IsLambda(false), NumBases(0), NumVBases(0), Bases(), VBases(),
    Definition(D), FirstFriend(0) {
}

CXXBaseSpecifier *CXXRecordDecl::DefinitionData::getBasesSlowCase() const {
  return Bases.get(Definition->getASTContext().getExternalSource());
}

CXXBaseSpecifier *CXXRecordDecl::DefinitionData::getVBasesSlowCase() const {
  return VBases.get(Definition->getASTContext().getExternalSource());
}

CXXRecordDecl::CXXRecordDecl(Kind K, TagKind TK, DeclContext *DC,
                             SourceLocation StartLoc, SourceLocation IdLoc,
                             IdentifierInfo *Id, CXXRecordDecl *PrevDecl)
  : RecordDecl(K, TK, DC, StartLoc, IdLoc, Id, PrevDecl),
    DefinitionData(PrevDecl ? PrevDecl->DefinitionData : 0),
    TemplateOrInstantiation() { }

CXXRecordDecl *CXXRecordDecl::Create(const ASTContext &C, TagKind TK,
                                     DeclContext *DC, SourceLocation StartLoc,
                                     SourceLocation IdLoc, IdentifierInfo *Id,
                                     CXXRecordDecl* PrevDecl,
                                     bool DelayTypeCreation) {
  CXXRecordDecl* R = new (C) CXXRecordDecl(CXXRecord, TK, DC, StartLoc, IdLoc,
                                           Id, PrevDecl);
  R->MayHaveOutOfDateDef = C.getLangOpts().Modules;

  // FIXME: DelayTypeCreation seems like such a hack
  if (!DelayTypeCreation)
    C.getTypeDeclType(R, PrevDecl);
  return R;
}

CXXRecordDecl *CXXRecordDecl::CreateLambda(const ASTContext &C, DeclContext *DC,
                                           TypeSourceInfo *Info, SourceLocation Loc,
                                           bool Dependent) {
  CXXRecordDecl* R = new (C) CXXRecordDecl(CXXRecord, TTK_Class, DC, Loc, Loc,
                                           0, 0);
  R->IsBeingDefined = true;
  R->DefinitionData = new (C) struct LambdaDefinitionData(R, Info, Dependent);
  R->MayHaveOutOfDateDef = false;
  C.getTypeDeclType(R, /*PrevDecl=*/0);
  return R;
}

CXXRecordDecl *
CXXRecordDecl::CreateDeserialized(const ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(CXXRecordDecl));
  CXXRecordDecl *R = new (Mem) CXXRecordDecl(CXXRecord, TTK_Struct, 0,
                                             SourceLocation(), SourceLocation(),
                                             0, 0);
  R->MayHaveOutOfDateDef = false;
  return R;
}

void
CXXRecordDecl::setBases(CXXBaseSpecifier const * const *Bases,
                        unsigned NumBases) {
  ASTContext &C = getASTContext();

  if (!data().Bases.isOffset() && data().NumBases > 0)
    C.Deallocate(data().getBases());

  if (NumBases) {
    // C++ [dcl.init.aggr]p1:
    //   An aggregate is [...] a class with [...] no base classes [...].
    data().Aggregate = false;

    // C++ [class]p4:
    //   A POD-struct is an aggregate class...
    data().PlainOldData = false;
  }

  // The set of seen virtual base types.
  llvm::SmallPtrSet<CanQualType, 8> SeenVBaseTypes;
  
  // The virtual bases of this class.
  SmallVector<const CXXBaseSpecifier *, 8> VBases;

  data().Bases = new(C) CXXBaseSpecifier [NumBases];
  data().NumBases = NumBases;
  for (unsigned i = 0; i < NumBases; ++i) {
    data().getBases()[i] = *Bases[i];
    // Keep track of inherited vbases for this base class.
    const CXXBaseSpecifier *Base = Bases[i];
    QualType BaseType = Base->getType();
    // Skip dependent types; we can't do any checking on them now.
    if (BaseType->isDependentType())
      continue;
    CXXRecordDecl *BaseClassDecl
      = cast<CXXRecordDecl>(BaseType->getAs<RecordType>()->getDecl());

    // A class with a non-empty base class is not empty.
    // FIXME: Standard ref?
    if (!BaseClassDecl->isEmpty()) {
      if (!data().Empty) {
        // C++0x [class]p7:
        //   A standard-layout class is a class that:
        //    [...]
        //    -- either has no non-static data members in the most derived
        //       class and at most one base class with non-static data members,
        //       or has no base classes with non-static data members, and
        // If this is the second non-empty base, then neither of these two
        // clauses can be true.
        data().IsStandardLayout = false;
      }

      data().Empty = false;
      data().HasNoNonEmptyBases = false;
    }
    
    // C++ [class.virtual]p1:
    //   A class that declares or inherits a virtual function is called a 
    //   polymorphic class.
    if (BaseClassDecl->isPolymorphic())
      data().Polymorphic = true;

    // C++0x [class]p7:
    //   A standard-layout class is a class that: [...]
    //    -- has no non-standard-layout base classes
    if (!BaseClassDecl->isStandardLayout())
      data().IsStandardLayout = false;

    // Record if this base is the first non-literal field or base.
    if (!hasNonLiteralTypeFieldsOrBases() && !BaseType->isLiteralType())
      data().HasNonLiteralTypeFieldsOrBases = true;
    
    // Now go through all virtual bases of this base and add them.
    for (CXXRecordDecl::base_class_iterator VBase =
          BaseClassDecl->vbases_begin(),
         E = BaseClassDecl->vbases_end(); VBase != E; ++VBase) {
      // Add this base if it's not already in the list.
      if (SeenVBaseTypes.insert(C.getCanonicalType(VBase->getType()))) {
        VBases.push_back(VBase);

        // C++11 [class.copy]p8:
        //   The implicitly-declared copy constructor for a class X will have
        //   the form 'X::X(const X&)' if each [...] virtual base class B of X
        //   has a copy constructor whose first parameter is of type
        //   'const B&' or 'const volatile B&' [...]
        if (CXXRecordDecl *VBaseDecl = VBase->getType()->getAsCXXRecordDecl())
          if (!VBaseDecl->hasCopyConstructorWithConstParam())
            data().ImplicitCopyConstructorHasConstParam = false;
      }
    }

    if (Base->isVirtual()) {
      // Add this base if it's not already in the list.
      if (SeenVBaseTypes.insert(C.getCanonicalType(BaseType)))
        VBases.push_back(Base);

      // C++0x [meta.unary.prop] is_empty:
      //    T is a class type, but not a union type, with ... no virtual base
      //    classes
      data().Empty = false;

      // C++11 [class.ctor]p5, C++11 [class.copy]p12, C++11 [class.copy]p25:
      //   A [default constructor, copy/move constructor, or copy/move assignment
      //   operator for a class X] is trivial [...] if:
      //    -- class X has [...] no virtual base classes
      data().HasTrivialSpecialMembers &= SMF_Destructor;

      // C++0x [class]p7:
      //   A standard-layout class is a class that: [...]
      //    -- has [...] no virtual base classes
      data().IsStandardLayout = false;

      // C++11 [dcl.constexpr]p4:
      //   In the definition of a constexpr constructor [...]
      //    -- the class shall not have any virtual base classes
      data().DefaultedDefaultConstructorIsConstexpr = false;
    } else {
      // C++ [class.ctor]p5:
      //   A default constructor is trivial [...] if:
      //    -- all the direct base classes of its class have trivial default
      //       constructors.
      if (!BaseClassDecl->hasTrivialDefaultConstructor())
        data().HasTrivialSpecialMembers &= ~SMF_DefaultConstructor;

      // C++0x [class.copy]p13:
      //   A copy/move constructor for class X is trivial if [...]
      //    [...]
      //    -- the constructor selected to copy/move each direct base class
      //       subobject is trivial, and
      if (!BaseClassDecl->hasTrivialCopyConstructor())
        data().HasTrivialSpecialMembers &= ~SMF_CopyConstructor;
      // If the base class doesn't have a simple move constructor, we'll eagerly
      // declare it and perform overload resolution to determine which function
      // it actually calls. If it does have a simple move constructor, this
      // check is correct.
      if (!BaseClassDecl->hasTrivialMoveConstructor())
        data().HasTrivialSpecialMembers &= ~SMF_MoveConstructor;

      // C++0x [class.copy]p27:
      //   A copy/move assignment operator for class X is trivial if [...]
      //    [...]
      //    -- the assignment operator selected to copy/move each direct base
      //       class subobject is trivial, and
      if (!BaseClassDecl->hasTrivialCopyAssignment())
        data().HasTrivialSpecialMembers &= ~SMF_CopyAssignment;
      // If the base class doesn't have a simple move assignment, we'll eagerly
      // declare it and perform overload resolution to determine which function
      // it actually calls. If it does have a simple move assignment, this
      // check is correct.
      if (!BaseClassDecl->hasTrivialMoveAssignment())
        data().HasTrivialSpecialMembers &= ~SMF_MoveAssignment;

      // C++11 [class.ctor]p6:
      //   If that user-written default constructor would satisfy the
      //   requirements of a constexpr constructor, the implicitly-defined
      //   default constructor is constexpr.
      if (!BaseClassDecl->hasConstexprDefaultConstructor())
        data().DefaultedDefaultConstructorIsConstexpr = false;
    }

    // C++ [class.ctor]p3:
    //   A destructor is trivial if all the direct base classes of its class
    //   have trivial destructors.
    if (!BaseClassDecl->hasTrivialDestructor())
      data().HasTrivialSpecialMembers &= ~SMF_Destructor;

    if (!BaseClassDecl->hasIrrelevantDestructor())
      data().HasIrrelevantDestructor = false;

    // C++11 [class.copy]p18:
    //   The implicitly-declared copy assignment oeprator for a class X will
    //   have the form 'X& X::operator=(const X&)' if each direct base class B
    //   of X has a copy assignment operator whose parameter is of type 'const
    //   B&', 'const volatile B&', or 'B' [...]
    if (!BaseClassDecl->hasCopyAssignmentWithConstParam())
      data().ImplicitCopyAssignmentHasConstParam = false;

    // C++11 [class.copy]p8:
    //   The implicitly-declared copy constructor for a class X will have
    //   the form 'X::X(const X&)' if each direct [...] base class B of X
    //   has a copy constructor whose first parameter is of type
    //   'const B&' or 'const volatile B&' [...]
    if (!BaseClassDecl->hasCopyConstructorWithConstParam())
      data().ImplicitCopyConstructorHasConstParam = false;

    // A class has an Objective-C object member if... or any of its bases
    // has an Objective-C object member.
    if (BaseClassDecl->hasObjectMember())
      setHasObjectMember(true);
    
    if (BaseClassDecl->hasVolatileMember())
      setHasVolatileMember(true);

    // Keep track of the presence of mutable fields.
    if (BaseClassDecl->hasMutableFields())
      data().HasMutableFields = true;

    if (BaseClassDecl->hasUninitializedReferenceMember())
      data().HasUninitializedReferenceMember = true;

    addedClassSubobject(BaseClassDecl);
  }
  
  if (VBases.empty())
    return;

  // Create base specifier for any direct or indirect virtual bases.
  data().VBases = new (C) CXXBaseSpecifier[VBases.size()];
  data().NumVBases = VBases.size();
  for (int I = 0, E = VBases.size(); I != E; ++I) {
    QualType Type = VBases[I]->getType();
    if (!Type->isDependentType())
      addedClassSubobject(Type->getAsCXXRecordDecl());
    data().getVBases()[I] = *VBases[I];
  }
}

void CXXRecordDecl::addedClassSubobject(CXXRecordDecl *Subobj) {
  // C++11 [class.copy]p11:
  //   A defaulted copy/move constructor for a class X is defined as
  //   deleted if X has:
  //    -- a direct or virtual base class B that cannot be copied/moved [...]
  //    -- a non-static data member of class type M (or array thereof)
  //       that cannot be copied or moved [...]
  if (!Subobj->hasSimpleMoveConstructor())
    data().NeedOverloadResolutionForMoveConstructor = true;

  // C++11 [class.copy]p23:
  //   A defaulted copy/move assignment operator for a class X is defined as
  //   deleted if X has:
  //    -- a direct or virtual base class B that cannot be copied/moved [...]
  //    -- a non-static data member of class type M (or array thereof)
  //        that cannot be copied or moved [...]
  if (!Subobj->hasSimpleMoveAssignment())
    data().NeedOverloadResolutionForMoveAssignment = true;

  // C++11 [class.ctor]p5, C++11 [class.copy]p11, C++11 [class.dtor]p5:
  //   A defaulted [ctor or dtor] for a class X is defined as
  //   deleted if X has:
  //    -- any direct or virtual base class [...] has a type with a destructor
  //       that is deleted or inaccessible from the defaulted [ctor or dtor].
  //    -- any non-static data member has a type with a destructor
  //       that is deleted or inaccessible from the defaulted [ctor or dtor].
  if (!Subobj->hasSimpleDestructor()) {
    data().NeedOverloadResolutionForMoveConstructor = true;
    data().NeedOverloadResolutionForDestructor = true;
  }
}

/// Callback function for CXXRecordDecl::forallBases that acknowledges
/// that it saw a base class.
static bool SawBase(const CXXRecordDecl *, void *) {
  return true;
}

bool CXXRecordDecl::hasAnyDependentBases() const {
  if (!isDependentContext())
    return false;

  return !forallBases(SawBase, 0);
}

bool CXXRecordDecl::isTriviallyCopyable() const {
  // C++0x [class]p5:
  //   A trivially copyable class is a class that:
  //   -- has no non-trivial copy constructors,
  if (hasNonTrivialCopyConstructor()) return false;
  //   -- has no non-trivial move constructors,
  if (hasNonTrivialMoveConstructor()) return false;
  //   -- has no non-trivial copy assignment operators,
  if (hasNonTrivialCopyAssignment()) return false;
  //   -- has no non-trivial move assignment operators, and
  if (hasNonTrivialMoveAssignment()) return false;
  //   -- has a trivial destructor.
  if (!hasTrivialDestructor()) return false;

  return true;
}

void CXXRecordDecl::markedVirtualFunctionPure() {
  // C++ [class.abstract]p2: 
  //   A class is abstract if it has at least one pure virtual function.
  data().Abstract = true;
}

void CXXRecordDecl::addedMember(Decl *D) {
  if (!D->isImplicit() &&
      !isa<FieldDecl>(D) &&
      !isa<IndirectFieldDecl>(D) &&
      (!isa<TagDecl>(D) || cast<TagDecl>(D)->getTagKind() == TTK_Class ||
        cast<TagDecl>(D)->getTagKind() == TTK_Interface))
    data().HasOnlyCMembers = false;

  // Ignore friends and invalid declarations.
  if (D->getFriendObjectKind() || D->isInvalidDecl())
    return;
  
  FunctionTemplateDecl *FunTmpl = dyn_cast<FunctionTemplateDecl>(D);
  if (FunTmpl)
    D = FunTmpl->getTemplatedDecl();
  
  if (CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(D)) {
    if (Method->isVirtual()) {
      // C++ [dcl.init.aggr]p1:
      //   An aggregate is an array or a class with [...] no virtual functions.
      data().Aggregate = false;
      
      // C++ [class]p4:
      //   A POD-struct is an aggregate class...
      data().PlainOldData = false;
      
      // Virtual functions make the class non-empty.
      // FIXME: Standard ref?
      data().Empty = false;

      // C++ [class.virtual]p1:
      //   A class that declares or inherits a virtual function is called a 
      //   polymorphic class.
      data().Polymorphic = true;

      // C++11 [class.ctor]p5, C++11 [class.copy]p12, C++11 [class.copy]p25:
      //   A [default constructor, copy/move constructor, or copy/move
      //   assignment operator for a class X] is trivial [...] if:
      //    -- class X has no virtual functions [...]
      data().HasTrivialSpecialMembers &= SMF_Destructor;

      // C++0x [class]p7:
      //   A standard-layout class is a class that: [...]
      //    -- has no virtual functions
      data().IsStandardLayout = false;
    }
  }

  // Notify the listener if an implicit member was added after the definition
  // was completed.
  if (!isBeingDefined() && D->isImplicit())
    if (ASTMutationListener *L = getASTMutationListener())
      L->AddedCXXImplicitMember(data().Definition, D);

  // The kind of special member this declaration is, if any.
  unsigned SMKind = 0;

  // Handle constructors.
  if (CXXConstructorDecl *Constructor = dyn_cast<CXXConstructorDecl>(D)) {
    if (!Constructor->isImplicit()) {
      // Note that we have a user-declared constructor.
      data().UserDeclaredConstructor = true;

      // C++ [class]p4:
      //   A POD-struct is an aggregate class [...]
      // Since the POD bit is meant to be C++03 POD-ness, clear it even if the
      // type is technically an aggregate in C++0x since it wouldn't be in 03.
      data().PlainOldData = false;
    }

    // Technically, "user-provided" is only defined for special member
    // functions, but the intent of the standard is clearly that it should apply
    // to all functions.
    bool UserProvided = Constructor->isUserProvided();

    if (Constructor->isDefaultConstructor()) {
      SMKind |= SMF_DefaultConstructor;

      if (UserProvided)
        data().UserProvidedDefaultConstructor = true;
      if (Constructor->isConstexpr())
        data().HasConstexprDefaultConstructor = true;
    }

    if (!FunTmpl) {
      unsigned Quals;
      if (Constructor->isCopyConstructor(Quals)) {
        SMKind |= SMF_CopyConstructor;

        if (Quals & Qualifiers::Const)
          data().HasDeclaredCopyConstructorWithConstParam = true;
      } else if (Constructor->isMoveConstructor())
        SMKind |= SMF_MoveConstructor;
    }

    // Record if we see any constexpr constructors which are neither copy
    // nor move constructors.
    if (Constructor->isConstexpr() && !Constructor->isCopyOrMoveConstructor())
      data().HasConstexprNonCopyMoveConstructor = true;

    // C++ [dcl.init.aggr]p1:
    //   An aggregate is an array or a class with no user-declared
    //   constructors [...].
    // C++11 [dcl.init.aggr]p1:
    //   An aggregate is an array or a class with no user-provided
    //   constructors [...].
    if (getASTContext().getLangOpts().CPlusPlus11
          ? UserProvided : !Constructor->isImplicit())
      data().Aggregate = false;
  }

  // Handle destructors.
  if (CXXDestructorDecl *DD = dyn_cast<CXXDestructorDecl>(D)) {
    SMKind |= SMF_Destructor;

    if (!DD->isImplicit())
      data().HasIrrelevantDestructor = false;

    // C++11 [class.dtor]p5:
    //   A destructor is trivial if [...] the destructor is not virtual.
    if (DD->isVirtual())
      data().HasTrivialSpecialMembers &= ~SMF_Destructor;
  }

  // Handle member functions.
  if (CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(D)) {
    if (Method->isCopyAssignmentOperator()) {
      SMKind |= SMF_CopyAssignment;

      const ReferenceType *ParamTy =
        Method->getParamDecl(0)->getType()->getAs<ReferenceType>();
      if (!ParamTy || ParamTy->getPointeeType().isConstQualified())
        data().HasDeclaredCopyAssignmentWithConstParam = true;
    }

    if (Method->isMoveAssignmentOperator())
      SMKind |= SMF_MoveAssignment;

    // Keep the list of conversion functions up-to-date.
    if (CXXConversionDecl *Conversion = dyn_cast<CXXConversionDecl>(D)) {
      // FIXME: We use the 'unsafe' accessor for the access specifier here,
      // because Sema may not have set it yet. That's really just a misdesign
      // in Sema. However, LLDB *will* have set the access specifier correctly,
      // and adds declarations after the class is technically completed,
      // so completeDefinition()'s overriding of the access specifiers doesn't
      // work.
      AccessSpecifier AS = Conversion->getAccessUnsafe();

      if (Conversion->getPrimaryTemplate()) {
        // We don't record specializations.
      } else if (FunTmpl) {
        if (FunTmpl->getPreviousDecl())
          data().Conversions.replace(FunTmpl->getPreviousDecl(),
                                     FunTmpl, AS);
        else
          data().Conversions.addDecl(getASTContext(), FunTmpl, AS);
      } else {
        if (Conversion->getPreviousDecl())
          data().Conversions.replace(Conversion->getPreviousDecl(),
                                     Conversion, AS);
        else
          data().Conversions.addDecl(getASTContext(), Conversion, AS);
      }
    }

    if (SMKind) {
      // If this is the first declaration of a special member, we no longer have
      // an implicit trivial special member.
      data().HasTrivialSpecialMembers &=
        data().DeclaredSpecialMembers | ~SMKind;

      if (!Method->isImplicit() && !Method->isUserProvided()) {
        // This method is user-declared but not user-provided. We can't work out
        // whether it's trivial yet (not until we get to the end of the class).
        // We'll handle this method in finishedDefaultedOrDeletedMember.
      } else if (Method->isTrivial())
        data().HasTrivialSpecialMembers |= SMKind;
      else
        data().DeclaredNonTrivialSpecialMembers |= SMKind;

      // Note when we have declared a declared special member, and suppress the
      // implicit declaration of this special member.
      data().DeclaredSpecialMembers |= SMKind;

      if (!Method->isImplicit()) {
        data().UserDeclaredSpecialMembers |= SMKind;

        // C++03 [class]p4:
        //   A POD-struct is an aggregate class that has [...] no user-defined
        //   copy assignment operator and no user-defined destructor.
        //
        // Since the POD bit is meant to be C++03 POD-ness, and in C++03,
        // aggregates could not have any constructors, clear it even for an
        // explicitly defaulted or deleted constructor.
        // type is technically an aggregate in C++0x since it wouldn't be in 03.
        //
        // Also, a user-declared move assignment operator makes a class non-POD.
        // This is an extension in C++03.
        data().PlainOldData = false;
      }
    }

    return;
  }

  // Handle non-static data members.
  if (FieldDecl *Field = dyn_cast<FieldDecl>(D)) {
    // C++ [class.bit]p2:
    //   A declaration for a bit-field that omits the identifier declares an 
    //   unnamed bit-field. Unnamed bit-fields are not members and cannot be 
    //   initialized.
    if (Field->isUnnamedBitfield())
      return;
    
    // C++ [dcl.init.aggr]p1:
    //   An aggregate is an array or a class (clause 9) with [...] no
    //   private or protected non-static data members (clause 11).
    //
    // A POD must be an aggregate.    
    if (D->getAccess() == AS_private || D->getAccess() == AS_protected) {
      data().Aggregate = false;
      data().PlainOldData = false;
    }

    // C++0x [class]p7:
    //   A standard-layout class is a class that:
    //    [...]
    //    -- has the same access control for all non-static data members,
    switch (D->getAccess()) {
    case AS_private:    data().HasPrivateFields = true;   break;
    case AS_protected:  data().HasProtectedFields = true; break;
    case AS_public:     data().HasPublicFields = true;    break;
    case AS_none:       llvm_unreachable("Invalid access specifier");
    };
    if ((data().HasPrivateFields + data().HasProtectedFields +
         data().HasPublicFields) > 1)
      data().IsStandardLayout = false;

    // Keep track of the presence of mutable fields.
    if (Field->isMutable())
      data().HasMutableFields = true;
    
    // C++0x [class]p9:
    //   A POD struct is a class that is both a trivial class and a 
    //   standard-layout class, and has no non-static data members of type 
    //   non-POD struct, non-POD union (or array of such types).
    //
    // Automatic Reference Counting: the presence of a member of Objective-C pointer type
    // that does not explicitly have no lifetime makes the class a non-POD.
    // However, we delay setting PlainOldData to false in this case so that
    // Sema has a chance to diagnostic causes where the same class will be
    // non-POD with Automatic Reference Counting but a POD without ARC.
    // In this case, the class will become a non-POD class when we complete
    // the definition.
    ASTContext &Context = getASTContext();
    QualType T = Context.getBaseElementType(Field->getType());
    if (T->isObjCRetainableType() || T.isObjCGCStrong()) {
      if (!Context.getLangOpts().ObjCAutoRefCount ||
          T.getObjCLifetime() != Qualifiers::OCL_ExplicitNone)
        setHasObjectMember(true);
    } else if (!T.isPODType(Context))
      data().PlainOldData = false;
    
    if (T->isReferenceType()) {
      if (!Field->hasInClassInitializer())
        data().HasUninitializedReferenceMember = true;

      // C++0x [class]p7:
      //   A standard-layout class is a class that:
      //    -- has no non-static data members of type [...] reference,
      data().IsStandardLayout = false;
    }

    // Record if this field is the first non-literal or volatile field or base.
    if (!T->isLiteralType() || T.isVolatileQualified())
      data().HasNonLiteralTypeFieldsOrBases = true;

    if (Field->hasInClassInitializer()) {
      data().HasInClassInitializer = true;

      // C++11 [class]p5:
      //   A default constructor is trivial if [...] no non-static data member
      //   of its class has a brace-or-equal-initializer.
      data().HasTrivialSpecialMembers &= ~SMF_DefaultConstructor;

      // C++11 [dcl.init.aggr]p1:
      //   An aggregate is a [...] class with [...] no
      //   brace-or-equal-initializers for non-static data members.
      //
      // This rule was removed in C++1y.
      if (!getASTContext().getLangOpts().CPlusPlus1y)
        data().Aggregate = false;

      // C++11 [class]p10:
      //   A POD struct is [...] a trivial class.
      data().PlainOldData = false;
    }

    // C++11 [class.copy]p23:
    //   A defaulted copy/move assignment operator for a class X is defined
    //   as deleted if X has:
    //    -- a non-static data member of reference type
    if (T->isReferenceType())
      data().DefaultedMoveAssignmentIsDeleted = true;

    if (const RecordType *RecordTy = T->getAs<RecordType>()) {
      CXXRecordDecl* FieldRec = cast<CXXRecordDecl>(RecordTy->getDecl());
      if (FieldRec->getDefinition()) {
        addedClassSubobject(FieldRec);

        // C++11 [class.ctor]p5, C++11 [class.copy]p11:
        //   A defaulted [special member] for a class X is defined as
        //   deleted if:
        //    -- X is a union-like class that has a variant member with a
        //       non-trivial [corresponding special member]
        if (isUnion()) {
          if (FieldRec->hasNonTrivialMoveConstructor())
            data().DefaultedMoveConstructorIsDeleted = true;
          if (FieldRec->hasNonTrivialMoveAssignment())
            data().DefaultedMoveAssignmentIsDeleted = true;
          if (FieldRec->hasNonTrivialDestructor())
            data().DefaultedDestructorIsDeleted = true;
        }

        // C++0x [class.ctor]p5:
        //   A default constructor is trivial [...] if:
        //    -- for all the non-static data members of its class that are of
        //       class type (or array thereof), each such class has a trivial
        //       default constructor.
        if (!FieldRec->hasTrivialDefaultConstructor())
          data().HasTrivialSpecialMembers &= ~SMF_DefaultConstructor;

        // C++0x [class.copy]p13:
        //   A copy/move constructor for class X is trivial if [...]
        //    [...]
        //    -- for each non-static data member of X that is of class type (or
        //       an array thereof), the constructor selected to copy/move that
        //       member is trivial;
        if (!FieldRec->hasTrivialCopyConstructor())
          data().HasTrivialSpecialMembers &= ~SMF_CopyConstructor;
        // If the field doesn't have a simple move constructor, we'll eagerly
        // declare the move constructor for this class and we'll decide whether
        // it's trivial then.
        if (!FieldRec->hasTrivialMoveConstructor())
          data().HasTrivialSpecialMembers &= ~SMF_MoveConstructor;

        // C++0x [class.copy]p27:
        //   A copy/move assignment operator for class X is trivial if [...]
        //    [...]
        //    -- for each non-static data member of X that is of class type (or
        //       an array thereof), the assignment operator selected to
        //       copy/move that member is trivial;
        if (!FieldRec->hasTrivialCopyAssignment())
          data().HasTrivialSpecialMembers &= ~SMF_CopyAssignment;
        // If the field doesn't have a simple move assignment, we'll eagerly
        // declare the move assignment for this class and we'll decide whether
        // it's trivial then.
        if (!FieldRec->hasTrivialMoveAssignment())
          data().HasTrivialSpecialMembers &= ~SMF_MoveAssignment;

        if (!FieldRec->hasTrivialDestructor())
          data().HasTrivialSpecialMembers &= ~SMF_Destructor;
        if (!FieldRec->hasIrrelevantDestructor())
          data().HasIrrelevantDestructor = false;
        if (FieldRec->hasObjectMember())
          setHasObjectMember(true);
        if (FieldRec->hasVolatileMember())
          setHasVolatileMember(true);

        // C++0x [class]p7:
        //   A standard-layout class is a class that:
        //    -- has no non-static data members of type non-standard-layout
        //       class (or array of such types) [...]
        if (!FieldRec->isStandardLayout())
          data().IsStandardLayout = false;

        // C++0x [class]p7:
        //   A standard-layout class is a class that:
        //    [...]
        //    -- has no base classes of the same type as the first non-static
        //       data member.
        // We don't want to expend bits in the state of the record decl
        // tracking whether this is the first non-static data member so we
        // cheat a bit and use some of the existing state: the empty bit.
        // Virtual bases and virtual methods make a class non-empty, but they
        // also make it non-standard-layout so we needn't check here.
        // A non-empty base class may leave the class standard-layout, but not
        // if we have arrived here, and have at least on non-static data
        // member. If IsStandardLayout remains true, then the first non-static
        // data member must come through here with Empty still true, and Empty
        // will subsequently be set to false below.
        if (data().IsStandardLayout && data().Empty) {
          for (CXXRecordDecl::base_class_const_iterator BI = bases_begin(),
                                                        BE = bases_end();
               BI != BE; ++BI) {
            if (Context.hasSameUnqualifiedType(BI->getType(), T)) {
              data().IsStandardLayout = false;
              break;
            }
          }
        }
        
        // Keep track of the presence of mutable fields.
        if (FieldRec->hasMutableFields())
          data().HasMutableFields = true;

        // C++11 [class.copy]p13:
        //   If the implicitly-defined constructor would satisfy the
        //   requirements of a constexpr constructor, the implicitly-defined
        //   constructor is constexpr.
        // C++11 [dcl.constexpr]p4:
        //    -- every constructor involved in initializing non-static data
        //       members [...] shall be a constexpr constructor
        if (!Field->hasInClassInitializer() &&
            !FieldRec->hasConstexprDefaultConstructor() && !isUnion())
          // The standard requires any in-class initializer to be a constant
          // expression. We consider this to be a defect.
          data().DefaultedDefaultConstructorIsConstexpr = false;

        // C++11 [class.copy]p8:
        //   The implicitly-declared copy constructor for a class X will have
        //   the form 'X::X(const X&)' if [...] for all the non-static data
        //   members of X that are of a class type M (or array thereof), each
        //   such class type has a copy constructor whose first parameter is
        //   of type 'const M&' or 'const volatile M&'.
        if (!FieldRec->hasCopyConstructorWithConstParam())
          data().ImplicitCopyConstructorHasConstParam = false;

        // C++11 [class.copy]p18:
        //   The implicitly-declared copy assignment oeprator for a class X will
        //   have the form 'X& X::operator=(const X&)' if [...] for all the
        //   non-static data members of X that are of a class type M (or array
        //   thereof), each such class type has a copy assignment operator whose
        //   parameter is of type 'const M&', 'const volatile M&' or 'M'.
        if (!FieldRec->hasCopyAssignmentWithConstParam())
          data().ImplicitCopyAssignmentHasConstParam = false;

        if (FieldRec->hasUninitializedReferenceMember() &&
            !Field->hasInClassInitializer())
          data().HasUninitializedReferenceMember = true;
      }
    } else {
      // Base element type of field is a non-class type.
      if (!T->isLiteralType() ||
          (!Field->hasInClassInitializer() && !isUnion()))
        data().DefaultedDefaultConstructorIsConstexpr = false;

      // C++11 [class.copy]p23:
      //   A defaulted copy/move assignment operator for a class X is defined
      //   as deleted if X has:
      //    -- a non-static data member of const non-class type (or array
      //       thereof)
      if (T.isConstQualified())
        data().DefaultedMoveAssignmentIsDeleted = true;
    }

    // C++0x [class]p7:
    //   A standard-layout class is a class that:
    //    [...]
    //    -- either has no non-static data members in the most derived
    //       class and at most one base class with non-static data members,
    //       or has no base classes with non-static data members, and
    // At this point we know that we have a non-static data member, so the last
    // clause holds.
    if (!data().HasNoNonEmptyBases)
      data().IsStandardLayout = false;

    // If this is not a zero-length bit-field, then the class is not empty.
    if (data().Empty) {
      if (!Field->isBitField() ||
          (!Field->getBitWidth()->isTypeDependent() &&
           !Field->getBitWidth()->isValueDependent() &&
           Field->getBitWidthValue(Context) != 0))
        data().Empty = false;
    }
  }
  
  // Handle using declarations of conversion functions.
  if (UsingShadowDecl *Shadow = dyn_cast<UsingShadowDecl>(D))
    if (Shadow->getDeclName().getNameKind()
          == DeclarationName::CXXConversionFunctionName)
      data().Conversions.addDecl(getASTContext(), Shadow, Shadow->getAccess());
}

void CXXRecordDecl::finishedDefaultedOrDeletedMember(CXXMethodDecl *D) {
  assert(!D->isImplicit() && !D->isUserProvided());

  // The kind of special member this declaration is, if any.
  unsigned SMKind = 0;

  if (CXXConstructorDecl *Constructor = dyn_cast<CXXConstructorDecl>(D)) {
    if (Constructor->isDefaultConstructor()) {
      SMKind |= SMF_DefaultConstructor;
      if (Constructor->isConstexpr())
        data().HasConstexprDefaultConstructor = true;
    }
    if (Constructor->isCopyConstructor())
      SMKind |= SMF_CopyConstructor;
    else if (Constructor->isMoveConstructor())
      SMKind |= SMF_MoveConstructor;
    else if (Constructor->isConstexpr())
      // We may now know that the constructor is constexpr.
      data().HasConstexprNonCopyMoveConstructor = true;
  } else if (isa<CXXDestructorDecl>(D))
    SMKind |= SMF_Destructor;
  else if (D->isCopyAssignmentOperator())
    SMKind |= SMF_CopyAssignment;
  else if (D->isMoveAssignmentOperator())
    SMKind |= SMF_MoveAssignment;

  // Update which trivial / non-trivial special members we have.
  // addedMember will have skipped this step for this member.
  if (D->isTrivial())
    data().HasTrivialSpecialMembers |= SMKind;
  else
    data().DeclaredNonTrivialSpecialMembers |= SMKind;
}

bool CXXRecordDecl::isCLike() const {
  if (getTagKind() == TTK_Class || getTagKind() == TTK_Interface ||
      !TemplateOrInstantiation.isNull())
    return false;
  if (!hasDefinition())
    return true;

  return isPOD() && data().HasOnlyCMembers;
}

void CXXRecordDecl::getCaptureFields(
       llvm::DenseMap<const VarDecl *, FieldDecl *> &Captures,
       FieldDecl *&ThisCapture) const {
  Captures.clear();
  ThisCapture = 0;

  LambdaDefinitionData &Lambda = getLambdaData();
  RecordDecl::field_iterator Field = field_begin();
  for (LambdaExpr::Capture *C = Lambda.Captures, *CEnd = C + Lambda.NumCaptures;
       C != CEnd; ++C, ++Field) {
    if (C->capturesThis()) {
      ThisCapture = *Field;
      continue;
    }

    Captures[C->getCapturedVar()] = *Field;
  }
}


static CanQualType GetConversionType(ASTContext &Context, NamedDecl *Conv) {
  QualType T;
  if (isa<UsingShadowDecl>(Conv))
    Conv = cast<UsingShadowDecl>(Conv)->getTargetDecl();
  if (FunctionTemplateDecl *ConvTemp = dyn_cast<FunctionTemplateDecl>(Conv))
    T = ConvTemp->getTemplatedDecl()->getResultType();
  else 
    T = cast<CXXConversionDecl>(Conv)->getConversionType();
  return Context.getCanonicalType(T);
}

/// Collect the visible conversions of a base class.
///
/// \param Record a base class of the class we're considering
/// \param InVirtual whether this base class is a virtual base (or a base
///   of a virtual base)
/// \param Access the access along the inheritance path to this base
/// \param ParentHiddenTypes the conversions provided by the inheritors
///   of this base
/// \param Output the set to which to add conversions from non-virtual bases
/// \param VOutput the set to which to add conversions from virtual bases
/// \param HiddenVBaseCs the set of conversions which were hidden in a
///   virtual base along some inheritance path
static void CollectVisibleConversions(ASTContext &Context,
                                      CXXRecordDecl *Record,
                                      bool InVirtual,
                                      AccessSpecifier Access,
                  const llvm::SmallPtrSet<CanQualType, 8> &ParentHiddenTypes,
                                      ASTUnresolvedSet &Output,
                                      UnresolvedSetImpl &VOutput,
                           llvm::SmallPtrSet<NamedDecl*, 8> &HiddenVBaseCs) {
  // The set of types which have conversions in this class or its
  // subclasses.  As an optimization, we don't copy the derived set
  // unless it might change.
  const llvm::SmallPtrSet<CanQualType, 8> *HiddenTypes = &ParentHiddenTypes;
  llvm::SmallPtrSet<CanQualType, 8> HiddenTypesBuffer;

  // Collect the direct conversions and figure out which conversions
  // will be hidden in the subclasses.
  CXXRecordDecl::conversion_iterator ConvI = Record->conversion_begin();
  CXXRecordDecl::conversion_iterator ConvE = Record->conversion_end();
  if (ConvI != ConvE) {
    HiddenTypesBuffer = ParentHiddenTypes;
    HiddenTypes = &HiddenTypesBuffer;

    for (CXXRecordDecl::conversion_iterator I = ConvI; I != ConvE; ++I) {
      CanQualType ConvType(GetConversionType(Context, I.getDecl()));
      bool Hidden = ParentHiddenTypes.count(ConvType);
      if (!Hidden)
        HiddenTypesBuffer.insert(ConvType);

      // If this conversion is hidden and we're in a virtual base,
      // remember that it's hidden along some inheritance path.
      if (Hidden && InVirtual)
        HiddenVBaseCs.insert(cast<NamedDecl>(I.getDecl()->getCanonicalDecl()));

      // If this conversion isn't hidden, add it to the appropriate output.
      else if (!Hidden) {
        AccessSpecifier IAccess
          = CXXRecordDecl::MergeAccess(Access, I.getAccess());

        if (InVirtual)
          VOutput.addDecl(I.getDecl(), IAccess);
        else
          Output.addDecl(Context, I.getDecl(), IAccess);
      }
    }
  }

  // Collect information recursively from any base classes.
  for (CXXRecordDecl::base_class_iterator
         I = Record->bases_begin(), E = Record->bases_end(); I != E; ++I) {
    const RecordType *RT = I->getType()->getAs<RecordType>();
    if (!RT) continue;

    AccessSpecifier BaseAccess
      = CXXRecordDecl::MergeAccess(Access, I->getAccessSpecifier());
    bool BaseInVirtual = InVirtual || I->isVirtual();

    CXXRecordDecl *Base = cast<CXXRecordDecl>(RT->getDecl());
    CollectVisibleConversions(Context, Base, BaseInVirtual, BaseAccess,
                              *HiddenTypes, Output, VOutput, HiddenVBaseCs);
  }
}

/// Collect the visible conversions of a class.
///
/// This would be extremely straightforward if it weren't for virtual
/// bases.  It might be worth special-casing that, really.
static void CollectVisibleConversions(ASTContext &Context,
                                      CXXRecordDecl *Record,
                                      ASTUnresolvedSet &Output) {
  // The collection of all conversions in virtual bases that we've
  // found.  These will be added to the output as long as they don't
  // appear in the hidden-conversions set.
  UnresolvedSet<8> VBaseCs;
  
  // The set of conversions in virtual bases that we've determined to
  // be hidden.
  llvm::SmallPtrSet<NamedDecl*, 8> HiddenVBaseCs;

  // The set of types hidden by classes derived from this one.
  llvm::SmallPtrSet<CanQualType, 8> HiddenTypes;

  // Go ahead and collect the direct conversions and add them to the
  // hidden-types set.
  CXXRecordDecl::conversion_iterator ConvI = Record->conversion_begin();
  CXXRecordDecl::conversion_iterator ConvE = Record->conversion_end();
  Output.append(Context, ConvI, ConvE);
  for (; ConvI != ConvE; ++ConvI)
    HiddenTypes.insert(GetConversionType(Context, ConvI.getDecl()));

  // Recursively collect conversions from base classes.
  for (CXXRecordDecl::base_class_iterator
         I = Record->bases_begin(), E = Record->bases_end(); I != E; ++I) {
    const RecordType *RT = I->getType()->getAs<RecordType>();
    if (!RT) continue;

    CollectVisibleConversions(Context, cast<CXXRecordDecl>(RT->getDecl()),
                              I->isVirtual(), I->getAccessSpecifier(),
                              HiddenTypes, Output, VBaseCs, HiddenVBaseCs);
  }

  // Add any unhidden conversions provided by virtual bases.
  for (UnresolvedSetIterator I = VBaseCs.begin(), E = VBaseCs.end();
         I != E; ++I) {
    if (!HiddenVBaseCs.count(cast<NamedDecl>(I.getDecl()->getCanonicalDecl())))
      Output.addDecl(Context, I.getDecl(), I.getAccess());
  }
}

/// getVisibleConversionFunctions - get all conversion functions visible
/// in current class; including conversion function templates.
std::pair<CXXRecordDecl::conversion_iterator,CXXRecordDecl::conversion_iterator>
CXXRecordDecl::getVisibleConversionFunctions() {
  // If root class, all conversions are visible.
  if (bases_begin() == bases_end())
    return std::make_pair(data().Conversions.begin(), data().Conversions.end());
  // If visible conversion list is already evaluated, return it.
  if (!data().ComputedVisibleConversions) {
    CollectVisibleConversions(getASTContext(), this, data().VisibleConversions);
    data().ComputedVisibleConversions = true;
  }
  return std::make_pair(data().VisibleConversions.begin(),
                        data().VisibleConversions.end());
}

void CXXRecordDecl::removeConversion(const NamedDecl *ConvDecl) {
  // This operation is O(N) but extremely rare.  Sema only uses it to
  // remove UsingShadowDecls in a class that were followed by a direct
  // declaration, e.g.:
  //   class A : B {
  //     using B::operator int;
  //     operator int();
  //   };
  // This is uncommon by itself and even more uncommon in conjunction
  // with sufficiently large numbers of directly-declared conversions
  // that asymptotic behavior matters.

  ASTUnresolvedSet &Convs = data().Conversions;
  for (unsigned I = 0, E = Convs.size(); I != E; ++I) {
    if (Convs[I].getDecl() == ConvDecl) {
      Convs.erase(I);
      assert(std::find(Convs.begin(), Convs.end(), ConvDecl) == Convs.end()
             && "conversion was found multiple times in unresolved set");
      return;
    }
  }

  llvm_unreachable("conversion not found in set!");
}

CXXRecordDecl *CXXRecordDecl::getInstantiatedFromMemberClass() const {
  if (MemberSpecializationInfo *MSInfo = getMemberSpecializationInfo())
    return cast<CXXRecordDecl>(MSInfo->getInstantiatedFrom());
  
  return 0;
}

void 
CXXRecordDecl::setInstantiationOfMemberClass(CXXRecordDecl *RD,
                                             TemplateSpecializationKind TSK) {
  assert(TemplateOrInstantiation.isNull() && 
         "Previous template or instantiation?");
  assert(!isa<ClassTemplateSpecializationDecl>(this));
  TemplateOrInstantiation 
    = new (getASTContext()) MemberSpecializationInfo(RD, TSK);
}

TemplateSpecializationKind CXXRecordDecl::getTemplateSpecializationKind() const{
  if (const ClassTemplateSpecializationDecl *Spec
        = dyn_cast<ClassTemplateSpecializationDecl>(this))
    return Spec->getSpecializationKind();
  
  if (MemberSpecializationInfo *MSInfo = getMemberSpecializationInfo())
    return MSInfo->getTemplateSpecializationKind();
  
  return TSK_Undeclared;
}

void 
CXXRecordDecl::setTemplateSpecializationKind(TemplateSpecializationKind TSK) {
  if (ClassTemplateSpecializationDecl *Spec
      = dyn_cast<ClassTemplateSpecializationDecl>(this)) {
    Spec->setSpecializationKind(TSK);
    return;
  }
  
  if (MemberSpecializationInfo *MSInfo = getMemberSpecializationInfo()) {
    MSInfo->setTemplateSpecializationKind(TSK);
    return;
  }
  
  llvm_unreachable("Not a class template or member class specialization");
}

CXXDestructorDecl *CXXRecordDecl::getDestructor() const {
  ASTContext &Context = getASTContext();
  QualType ClassType = Context.getTypeDeclType(this);

  DeclarationName Name
    = Context.DeclarationNames.getCXXDestructorName(
                                          Context.getCanonicalType(ClassType));

  DeclContext::lookup_const_result R = lookup(Name);
  if (R.empty())
    return 0;

  CXXDestructorDecl *Dtor = cast<CXXDestructorDecl>(R.front());
  return Dtor;
}

void CXXRecordDecl::completeDefinition() {
  completeDefinition(0);
}

void CXXRecordDecl::completeDefinition(CXXFinalOverriderMap *FinalOverriders) {
  RecordDecl::completeDefinition();
  
  if (hasObjectMember() && getASTContext().getLangOpts().ObjCAutoRefCount) {
    // Objective-C Automatic Reference Counting:
    //   If a class has a non-static data member of Objective-C pointer
    //   type (or array thereof), it is a non-POD type and its
    //   default constructor (if any), copy constructor, move constructor,
    //   copy assignment operator, move assignment operator, and destructor are
    //   non-trivial.
    struct DefinitionData &Data = data();
    Data.PlainOldData = false;
    Data.HasTrivialSpecialMembers = 0;
    Data.HasIrrelevantDestructor = false;
  }
  
  // If the class may be abstract (but hasn't been marked as such), check for
  // any pure final overriders.
  if (mayBeAbstract()) {
    CXXFinalOverriderMap MyFinalOverriders;
    if (!FinalOverriders) {
      getFinalOverriders(MyFinalOverriders);
      FinalOverriders = &MyFinalOverriders;
    }
    
    bool Done = false;
    for (CXXFinalOverriderMap::iterator M = FinalOverriders->begin(), 
                                     MEnd = FinalOverriders->end();
         M != MEnd && !Done; ++M) {
      for (OverridingMethods::iterator SO = M->second.begin(), 
                                    SOEnd = M->second.end();
           SO != SOEnd && !Done; ++SO) {
        assert(SO->second.size() > 0 && 
               "All virtual functions have overridding virtual functions");
        
        // C++ [class.abstract]p4:
        //   A class is abstract if it contains or inherits at least one
        //   pure virtual function for which the final overrider is pure
        //   virtual.
        if (SO->second.front().Method->isPure()) {
          data().Abstract = true;
          Done = true;
          break;
        }
      }
    }
  }
  
  // Set access bits correctly on the directly-declared conversions.
  for (UnresolvedSetIterator I = data().Conversions.begin(), 
                             E = data().Conversions.end(); 
       I != E; ++I)
    I.setAccess((*I)->getAccess());
}

bool CXXRecordDecl::mayBeAbstract() const {
  if (data().Abstract || isInvalidDecl() || !data().Polymorphic ||
      isDependentContext())
    return false;
  
  for (CXXRecordDecl::base_class_const_iterator B = bases_begin(),
                                             BEnd = bases_end();
       B != BEnd; ++B) {
    CXXRecordDecl *BaseDecl 
      = cast<CXXRecordDecl>(B->getType()->getAs<RecordType>()->getDecl());
    if (BaseDecl->isAbstract())
      return true;
  }
  
  return false;
}

void CXXMethodDecl::anchor() { }

bool CXXMethodDecl::isStatic() const {
  const CXXMethodDecl *MD = getCanonicalDecl();

  if (MD->getStorageClass() == SC_Static)
    return true;

  DeclarationName Name = getDeclName();
  // [class.free]p1:
  // Any allocation function for a class T is a static member
  // (even if not explicitly declared static).
  if (Name.getCXXOverloadedOperator() == OO_New ||
      Name.getCXXOverloadedOperator() == OO_Array_New)
    return true;

  // [class.free]p6 Any deallocation function for a class X is a static member
  // (even if not explicitly declared static).
  if (Name.getCXXOverloadedOperator() == OO_Delete ||
      Name.getCXXOverloadedOperator() == OO_Array_Delete)
    return true;

  return false;
}

static bool recursivelyOverrides(const CXXMethodDecl *DerivedMD,
                                 const CXXMethodDecl *BaseMD) {
  for (CXXMethodDecl::method_iterator I = DerivedMD->begin_overridden_methods(),
         E = DerivedMD->end_overridden_methods(); I != E; ++I) {
    const CXXMethodDecl *MD = *I;
    if (MD->getCanonicalDecl() == BaseMD->getCanonicalDecl())
      return true;
    if (recursivelyOverrides(MD, BaseMD))
      return true;
  }
  return false;
}

CXXMethodDecl *
CXXMethodDecl::getCorrespondingMethodInClass(const CXXRecordDecl *RD,
                                             bool MayBeBase) {
  if (this->getParent()->getCanonicalDecl() == RD->getCanonicalDecl())
    return this;

  // Lookup doesn't work for destructors, so handle them separately.
  if (isa<CXXDestructorDecl>(this)) {
    CXXMethodDecl *MD = RD->getDestructor();
    if (MD) {
      if (recursivelyOverrides(MD, this))
        return MD;
      if (MayBeBase && recursivelyOverrides(this, MD))
        return MD;
    }
    return NULL;
  }

  lookup_const_result Candidates = RD->lookup(getDeclName());
  for (NamedDecl * const * I = Candidates.begin(); I != Candidates.end(); ++I) {
    CXXMethodDecl *MD = dyn_cast<CXXMethodDecl>(*I);
    if (!MD)
      continue;
    if (recursivelyOverrides(MD, this))
      return MD;
    if (MayBeBase && recursivelyOverrides(this, MD))
      return MD;
  }

  for (CXXRecordDecl::base_class_const_iterator I = RD->bases_begin(),
         E = RD->bases_end(); I != E; ++I) {
    const RecordType *RT = I->getType()->getAs<RecordType>();
    if (!RT)
      continue;
    const CXXRecordDecl *Base = cast<CXXRecordDecl>(RT->getDecl());
    CXXMethodDecl *T = this->getCorrespondingMethodInClass(Base);
    if (T)
      return T;
  }

  return NULL;
}

CXXMethodDecl *
CXXMethodDecl::Create(ASTContext &C, CXXRecordDecl *RD,
                      SourceLocation StartLoc,
                      const DeclarationNameInfo &NameInfo,
                      QualType T, TypeSourceInfo *TInfo,
                      StorageClass SC, bool isInline,
                      bool isConstexpr, SourceLocation EndLocation) {
  return new (C) CXXMethodDecl(CXXMethod, RD, StartLoc, NameInfo, T, TInfo,
                               SC, isInline, isConstexpr,
                               EndLocation);
}

CXXMethodDecl *CXXMethodDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(CXXMethodDecl));
  return new (Mem) CXXMethodDecl(CXXMethod, 0, SourceLocation(), 
                                 DeclarationNameInfo(), QualType(),
                                 0, SC_None, false, false,
                                 SourceLocation());
}

bool CXXMethodDecl::isUsualDeallocationFunction() const {
  if (getOverloadedOperator() != OO_Delete &&
      getOverloadedOperator() != OO_Array_Delete)
    return false;

  // C++ [basic.stc.dynamic.deallocation]p2:
  //   A template instance is never a usual deallocation function,
  //   regardless of its signature.
  if (getPrimaryTemplate())
    return false;

  // C++ [basic.stc.dynamic.deallocation]p2:
  //   If a class T has a member deallocation function named operator delete 
  //   with exactly one parameter, then that function is a usual (non-placement)
  //   deallocation function. [...]
  if (getNumParams() == 1)
    return true;
  
  // C++ [basic.stc.dynamic.deallocation]p2:
  //   [...] If class T does not declare such an operator delete but does 
  //   declare a member deallocation function named operator delete with 
  //   exactly two parameters, the second of which has type std::size_t (18.1),
  //   then this function is a usual deallocation function.
  ASTContext &Context = getASTContext();
  if (getNumParams() != 2 ||
      !Context.hasSameUnqualifiedType(getParamDecl(1)->getType(),
                                      Context.getSizeType()))
    return false;
                 
  // This function is a usual deallocation function if there are no 
  // single-parameter deallocation functions of the same kind.
  DeclContext::lookup_const_result R = getDeclContext()->lookup(getDeclName());
  for (DeclContext::lookup_const_result::iterator I = R.begin(), E = R.end();
       I != E; ++I) {
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(*I))
      if (FD->getNumParams() == 1)
        return false;
  }
  
  return true;
}

bool CXXMethodDecl::isCopyAssignmentOperator() const {
  // C++0x [class.copy]p17:
  //  A user-declared copy assignment operator X::operator= is a non-static 
  //  non-template member function of class X with exactly one parameter of 
  //  type X, X&, const X&, volatile X& or const volatile X&.
  if (/*operator=*/getOverloadedOperator() != OO_Equal ||
      /*non-static*/ isStatic() || 
      /*non-template*/getPrimaryTemplate() || getDescribedFunctionTemplate())
    return false;
      
  QualType ParamType = getParamDecl(0)->getType();
  if (const LValueReferenceType *Ref = ParamType->getAs<LValueReferenceType>())
    ParamType = Ref->getPointeeType();
  
  ASTContext &Context = getASTContext();
  QualType ClassType
    = Context.getCanonicalType(Context.getTypeDeclType(getParent()));
  return Context.hasSameUnqualifiedType(ClassType, ParamType);
}

bool CXXMethodDecl::isMoveAssignmentOperator() const {
  // C++0x [class.copy]p19:
  //  A user-declared move assignment operator X::operator= is a non-static
  //  non-template member function of class X with exactly one parameter of type
  //  X&&, const X&&, volatile X&&, or const volatile X&&.
  if (getOverloadedOperator() != OO_Equal || isStatic() ||
      getPrimaryTemplate() || getDescribedFunctionTemplate())
    return false;

  QualType ParamType = getParamDecl(0)->getType();
  if (!isa<RValueReferenceType>(ParamType))
    return false;
  ParamType = ParamType->getPointeeType();

  ASTContext &Context = getASTContext();
  QualType ClassType
    = Context.getCanonicalType(Context.getTypeDeclType(getParent()));
  return Context.hasSameUnqualifiedType(ClassType, ParamType);
}

void CXXMethodDecl::addOverriddenMethod(const CXXMethodDecl *MD) {
  assert(MD->isCanonicalDecl() && "Method is not canonical!");
  assert(!MD->getParent()->isDependentContext() &&
         "Can't add an overridden method to a class template!");
  assert(MD->isVirtual() && "Method is not virtual!");

  getASTContext().addOverriddenMethod(this, MD);
}

CXXMethodDecl::method_iterator CXXMethodDecl::begin_overridden_methods() const {
  if (isa<CXXConstructorDecl>(this)) return 0;
  return getASTContext().overridden_methods_begin(this);
}

CXXMethodDecl::method_iterator CXXMethodDecl::end_overridden_methods() const {
  if (isa<CXXConstructorDecl>(this)) return 0;
  return getASTContext().overridden_methods_end(this);
}

unsigned CXXMethodDecl::size_overridden_methods() const {
  if (isa<CXXConstructorDecl>(this)) return 0;
  return getASTContext().overridden_methods_size(this);
}

QualType CXXMethodDecl::getThisType(ASTContext &C) const {
  // C++ 9.3.2p1: The type of this in a member function of a class X is X*.
  // If the member function is declared const, the type of this is const X*,
  // if the member function is declared volatile, the type of this is
  // volatile X*, and if the member function is declared const volatile,
  // the type of this is const volatile X*.

  assert(isInstance() && "No 'this' for static methods!");

  QualType ClassTy = C.getTypeDeclType(getParent());
  ClassTy = C.getQualifiedType(ClassTy,
                               Qualifiers::fromCVRMask(getTypeQualifiers()));
  return C.getPointerType(ClassTy);
}

bool CXXMethodDecl::hasInlineBody() const {
  // If this function is a template instantiation, look at the template from 
  // which it was instantiated.
  const FunctionDecl *CheckFn = getTemplateInstantiationPattern();
  if (!CheckFn)
    CheckFn = this;
  
  const FunctionDecl *fn;
  return CheckFn->hasBody(fn) && !fn->isOutOfLine();
}

bool CXXMethodDecl::isLambdaStaticInvoker() const {
  return getParent()->isLambda() && 
         getIdentifier() && getIdentifier()->getName() == "__invoke";
}


CXXCtorInitializer::CXXCtorInitializer(ASTContext &Context,
                                       TypeSourceInfo *TInfo, bool IsVirtual,
                                       SourceLocation L, Expr *Init,
                                       SourceLocation R,
                                       SourceLocation EllipsisLoc)
  : Initializee(TInfo), MemberOrEllipsisLocation(EllipsisLoc), Init(Init), 
    LParenLoc(L), RParenLoc(R), IsDelegating(false), IsVirtual(IsVirtual), 
    IsWritten(false), SourceOrderOrNumArrayIndices(0)
{
}

CXXCtorInitializer::CXXCtorInitializer(ASTContext &Context,
                                       FieldDecl *Member,
                                       SourceLocation MemberLoc,
                                       SourceLocation L, Expr *Init,
                                       SourceLocation R)
  : Initializee(Member), MemberOrEllipsisLocation(MemberLoc), Init(Init),
    LParenLoc(L), RParenLoc(R), IsDelegating(false), IsVirtual(false),
    IsWritten(false), SourceOrderOrNumArrayIndices(0)
{
}

CXXCtorInitializer::CXXCtorInitializer(ASTContext &Context,
                                       IndirectFieldDecl *Member,
                                       SourceLocation MemberLoc,
                                       SourceLocation L, Expr *Init,
                                       SourceLocation R)
  : Initializee(Member), MemberOrEllipsisLocation(MemberLoc), Init(Init),
    LParenLoc(L), RParenLoc(R), IsDelegating(false), IsVirtual(false),
    IsWritten(false), SourceOrderOrNumArrayIndices(0)
{
}

CXXCtorInitializer::CXXCtorInitializer(ASTContext &Context,
                                       TypeSourceInfo *TInfo,
                                       SourceLocation L, Expr *Init, 
                                       SourceLocation R)
  : Initializee(TInfo), MemberOrEllipsisLocation(), Init(Init),
    LParenLoc(L), RParenLoc(R), IsDelegating(true), IsVirtual(false),
    IsWritten(false), SourceOrderOrNumArrayIndices(0)
{
}

CXXCtorInitializer::CXXCtorInitializer(ASTContext &Context,
                                       FieldDecl *Member,
                                       SourceLocation MemberLoc,
                                       SourceLocation L, Expr *Init,
                                       SourceLocation R,
                                       VarDecl **Indices,
                                       unsigned NumIndices)
  : Initializee(Member), MemberOrEllipsisLocation(MemberLoc), Init(Init), 
    LParenLoc(L), RParenLoc(R), IsVirtual(false),
    IsWritten(false), SourceOrderOrNumArrayIndices(NumIndices)
{
  VarDecl **MyIndices = reinterpret_cast<VarDecl **> (this + 1);
  memcpy(MyIndices, Indices, NumIndices * sizeof(VarDecl *));
}

CXXCtorInitializer *CXXCtorInitializer::Create(ASTContext &Context,
                                               FieldDecl *Member, 
                                               SourceLocation MemberLoc,
                                               SourceLocation L, Expr *Init,
                                               SourceLocation R,
                                               VarDecl **Indices,
                                               unsigned NumIndices) {
  void *Mem = Context.Allocate(sizeof(CXXCtorInitializer) +
                               sizeof(VarDecl *) * NumIndices,
                               llvm::alignOf<CXXCtorInitializer>());
  return new (Mem) CXXCtorInitializer(Context, Member, MemberLoc, L, Init, R,
                                      Indices, NumIndices);
}

TypeLoc CXXCtorInitializer::getBaseClassLoc() const {
  if (isBaseInitializer())
    return Initializee.get<TypeSourceInfo*>()->getTypeLoc();
  else
    return TypeLoc();
}

const Type *CXXCtorInitializer::getBaseClass() const {
  if (isBaseInitializer())
    return Initializee.get<TypeSourceInfo*>()->getType().getTypePtr();
  else
    return 0;
}

SourceLocation CXXCtorInitializer::getSourceLocation() const {
  if (isAnyMemberInitializer())
    return getMemberLocation();

  if (isInClassMemberInitializer())
    return getAnyMember()->getLocation();
  
  if (TypeSourceInfo *TSInfo = Initializee.get<TypeSourceInfo*>())
    return TSInfo->getTypeLoc().getLocalSourceRange().getBegin();
  
  return SourceLocation();
}

SourceRange CXXCtorInitializer::getSourceRange() const {
  if (isInClassMemberInitializer()) {
    FieldDecl *D = getAnyMember();
    if (Expr *I = D->getInClassInitializer())
      return I->getSourceRange();
    return SourceRange();
  }

  return SourceRange(getSourceLocation(), getRParenLoc());
}

void CXXConstructorDecl::anchor() { }

CXXConstructorDecl *
CXXConstructorDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(CXXConstructorDecl));
  return new (Mem) CXXConstructorDecl(0, SourceLocation(),DeclarationNameInfo(),
                                      QualType(), 0, false, false, false,false);
}

CXXConstructorDecl *
CXXConstructorDecl::Create(ASTContext &C, CXXRecordDecl *RD,
                           SourceLocation StartLoc,
                           const DeclarationNameInfo &NameInfo,
                           QualType T, TypeSourceInfo *TInfo,
                           bool isExplicit, bool isInline,
                           bool isImplicitlyDeclared, bool isConstexpr) {
  assert(NameInfo.getName().getNameKind()
         == DeclarationName::CXXConstructorName &&
         "Name must refer to a constructor");
  return new (C) CXXConstructorDecl(RD, StartLoc, NameInfo, T, TInfo,
                                    isExplicit, isInline, isImplicitlyDeclared,
                                    isConstexpr);
}

CXXConstructorDecl *CXXConstructorDecl::getTargetConstructor() const {
  assert(isDelegatingConstructor() && "Not a delegating constructor!");
  Expr *E = (*init_begin())->getInit()->IgnoreImplicit();
  if (CXXConstructExpr *Construct = dyn_cast<CXXConstructExpr>(E))
    return Construct->getConstructor();
  
  return 0;
}

bool CXXConstructorDecl::isDefaultConstructor() const {
  // C++ [class.ctor]p5:
  //   A default constructor for a class X is a constructor of class
  //   X that can be called without an argument.
  return (getNumParams() == 0) ||
         (getNumParams() > 0 && getParamDecl(0)->hasDefaultArg());
}

bool
CXXConstructorDecl::isCopyConstructor(unsigned &TypeQuals) const {
  return isCopyOrMoveConstructor(TypeQuals) &&
         getParamDecl(0)->getType()->isLValueReferenceType();
}

bool CXXConstructorDecl::isMoveConstructor(unsigned &TypeQuals) const {
  return isCopyOrMoveConstructor(TypeQuals) &&
    getParamDecl(0)->getType()->isRValueReferenceType();
}

/// \brief Determine whether this is a copy or move constructor.
bool CXXConstructorDecl::isCopyOrMoveConstructor(unsigned &TypeQuals) const {
  // C++ [class.copy]p2:
  //   A non-template constructor for class X is a copy constructor
  //   if its first parameter is of type X&, const X&, volatile X& or
  //   const volatile X&, and either there are no other parameters
  //   or else all other parameters have default arguments (8.3.6).
  // C++0x [class.copy]p3:
  //   A non-template constructor for class X is a move constructor if its
  //   first parameter is of type X&&, const X&&, volatile X&&, or 
  //   const volatile X&&, and either there are no other parameters or else 
  //   all other parameters have default arguments.
  if ((getNumParams() < 1) ||
      (getNumParams() > 1 && !getParamDecl(1)->hasDefaultArg()) ||
      (getPrimaryTemplate() != 0) ||
      (getDescribedFunctionTemplate() != 0))
    return false;
  
  const ParmVarDecl *Param = getParamDecl(0);
  
  // Do we have a reference type? 
  const ReferenceType *ParamRefType = Param->getType()->getAs<ReferenceType>();
  if (!ParamRefType)
    return false;
  
  // Is it a reference to our class type?
  ASTContext &Context = getASTContext();
  
  CanQualType PointeeType
    = Context.getCanonicalType(ParamRefType->getPointeeType());
  CanQualType ClassTy 
    = Context.getCanonicalType(Context.getTagDeclType(getParent()));
  if (PointeeType.getUnqualifiedType() != ClassTy)
    return false;
  
  // FIXME: other qualifiers?
  
  // We have a copy or move constructor.
  TypeQuals = PointeeType.getCVRQualifiers();
  return true;  
}

bool CXXConstructorDecl::isConvertingConstructor(bool AllowExplicit) const {
  // C++ [class.conv.ctor]p1:
  //   A constructor declared without the function-specifier explicit
  //   that can be called with a single parameter specifies a
  //   conversion from the type of its first parameter to the type of
  //   its class. Such a constructor is called a converting
  //   constructor.
  if (isExplicit() && !AllowExplicit)
    return false;

  return (getNumParams() == 0 &&
          getType()->getAs<FunctionProtoType>()->isVariadic()) ||
         (getNumParams() == 1) ||
         (getNumParams() > 1 &&
          (getParamDecl(1)->hasDefaultArg() ||
           getParamDecl(1)->isParameterPack()));
}

bool CXXConstructorDecl::isSpecializationCopyingObject() const {
  if ((getNumParams() < 1) ||
      (getNumParams() > 1 && !getParamDecl(1)->hasDefaultArg()) ||
      (getPrimaryTemplate() == 0) ||
      (getDescribedFunctionTemplate() != 0))
    return false;

  const ParmVarDecl *Param = getParamDecl(0);

  ASTContext &Context = getASTContext();
  CanQualType ParamType = Context.getCanonicalType(Param->getType());
  
  // Is it the same as our our class type?
  CanQualType ClassTy 
    = Context.getCanonicalType(Context.getTagDeclType(getParent()));
  if (ParamType.getUnqualifiedType() != ClassTy)
    return false;
  
  return true;  
}

const CXXConstructorDecl *CXXConstructorDecl::getInheritedConstructor() const {
  // Hack: we store the inherited constructor in the overridden method table
  method_iterator It = getASTContext().overridden_methods_begin(this);
  if (It == getASTContext().overridden_methods_end(this))
    return 0;

  return cast<CXXConstructorDecl>(*It);
}

void
CXXConstructorDecl::setInheritedConstructor(const CXXConstructorDecl *BaseCtor){
  // Hack: we store the inherited constructor in the overridden method table
  assert(getASTContext().overridden_methods_size(this) == 0 &&
         "Base ctor already set.");
  getASTContext().addOverriddenMethod(this, BaseCtor);
}

void CXXDestructorDecl::anchor() { }

CXXDestructorDecl *
CXXDestructorDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(CXXDestructorDecl));
  return new (Mem) CXXDestructorDecl(0, SourceLocation(), DeclarationNameInfo(),
                                   QualType(), 0, false, false);
}

CXXDestructorDecl *
CXXDestructorDecl::Create(ASTContext &C, CXXRecordDecl *RD,
                          SourceLocation StartLoc,
                          const DeclarationNameInfo &NameInfo,
                          QualType T, TypeSourceInfo *TInfo,
                          bool isInline, bool isImplicitlyDeclared) {
  assert(NameInfo.getName().getNameKind()
         == DeclarationName::CXXDestructorName &&
         "Name must refer to a destructor");
  return new (C) CXXDestructorDecl(RD, StartLoc, NameInfo, T, TInfo, isInline,
                                   isImplicitlyDeclared);
}

void CXXConversionDecl::anchor() { }

CXXConversionDecl *
CXXConversionDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(CXXConversionDecl));
  return new (Mem) CXXConversionDecl(0, SourceLocation(), DeclarationNameInfo(),
                                     QualType(), 0, false, false, false,
                                     SourceLocation());
}

CXXConversionDecl *
CXXConversionDecl::Create(ASTContext &C, CXXRecordDecl *RD,
                          SourceLocation StartLoc,
                          const DeclarationNameInfo &NameInfo,
                          QualType T, TypeSourceInfo *TInfo,
                          bool isInline, bool isExplicit,
                          bool isConstexpr, SourceLocation EndLocation) {
  assert(NameInfo.getName().getNameKind()
         == DeclarationName::CXXConversionFunctionName &&
         "Name must refer to a conversion function");
  return new (C) CXXConversionDecl(RD, StartLoc, NameInfo, T, TInfo,
                                   isInline, isExplicit, isConstexpr,
                                   EndLocation);
}

bool CXXConversionDecl::isLambdaToBlockPointerConversion() const {
  return isImplicit() && getParent()->isLambda() &&
         getConversionType()->isBlockPointerType();
}

void LinkageSpecDecl::anchor() { }

LinkageSpecDecl *LinkageSpecDecl::Create(ASTContext &C,
                                         DeclContext *DC,
                                         SourceLocation ExternLoc,
                                         SourceLocation LangLoc,
                                         LanguageIDs Lang,
                                         SourceLocation RBraceLoc) {
  return new (C) LinkageSpecDecl(DC, ExternLoc, LangLoc, Lang, RBraceLoc);
}

LinkageSpecDecl *LinkageSpecDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(LinkageSpecDecl));
  return new (Mem) LinkageSpecDecl(0, SourceLocation(), SourceLocation(),
                                   lang_c, SourceLocation());
}

void UsingDirectiveDecl::anchor() { }

UsingDirectiveDecl *UsingDirectiveDecl::Create(ASTContext &C, DeclContext *DC,
                                               SourceLocation L,
                                               SourceLocation NamespaceLoc,
                                           NestedNameSpecifierLoc QualifierLoc,
                                               SourceLocation IdentLoc,
                                               NamedDecl *Used,
                                               DeclContext *CommonAncestor) {
  if (NamespaceDecl *NS = dyn_cast_or_null<NamespaceDecl>(Used))
    Used = NS->getOriginalNamespace();
  return new (C) UsingDirectiveDecl(DC, L, NamespaceLoc, QualifierLoc,
                                    IdentLoc, Used, CommonAncestor);
}

UsingDirectiveDecl *
UsingDirectiveDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(UsingDirectiveDecl));
  return new (Mem) UsingDirectiveDecl(0, SourceLocation(), SourceLocation(),
                                      NestedNameSpecifierLoc(),
                                      SourceLocation(), 0, 0);
}

NamespaceDecl *UsingDirectiveDecl::getNominatedNamespace() {
  if (NamespaceAliasDecl *NA =
        dyn_cast_or_null<NamespaceAliasDecl>(NominatedNamespace))
    return NA->getNamespace();
  return cast_or_null<NamespaceDecl>(NominatedNamespace);
}

void NamespaceDecl::anchor() { }

NamespaceDecl::NamespaceDecl(DeclContext *DC, bool Inline, 
                             SourceLocation StartLoc,
                             SourceLocation IdLoc, IdentifierInfo *Id,
                             NamespaceDecl *PrevDecl)
  : NamedDecl(Namespace, DC, IdLoc, Id), DeclContext(Namespace),
    LocStart(StartLoc), RBraceLoc(), AnonOrFirstNamespaceAndInline(0, Inline) 
{
  setPreviousDeclaration(PrevDecl);
  
  if (PrevDecl)
    AnonOrFirstNamespaceAndInline.setPointer(PrevDecl->getOriginalNamespace());
}

NamespaceDecl *NamespaceDecl::Create(ASTContext &C, DeclContext *DC,
                                     bool Inline, SourceLocation StartLoc,
                                     SourceLocation IdLoc, IdentifierInfo *Id,
                                     NamespaceDecl *PrevDecl) {
  return new (C) NamespaceDecl(DC, Inline, StartLoc, IdLoc, Id, PrevDecl);
}

NamespaceDecl *NamespaceDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(NamespaceDecl));
  return new (Mem) NamespaceDecl(0, false, SourceLocation(), SourceLocation(), 
                                 0, 0);
}

void NamespaceAliasDecl::anchor() { }

NamespaceAliasDecl *NamespaceAliasDecl::Create(ASTContext &C, DeclContext *DC,
                                               SourceLocation UsingLoc,
                                               SourceLocation AliasLoc,
                                               IdentifierInfo *Alias,
                                           NestedNameSpecifierLoc QualifierLoc,
                                               SourceLocation IdentLoc,
                                               NamedDecl *Namespace) {
  if (NamespaceDecl *NS = dyn_cast_or_null<NamespaceDecl>(Namespace))
    Namespace = NS->getOriginalNamespace();
  return new (C) NamespaceAliasDecl(DC, UsingLoc, AliasLoc, Alias, 
                                    QualifierLoc, IdentLoc, Namespace);
}

NamespaceAliasDecl *
NamespaceAliasDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(NamespaceAliasDecl));
  return new (Mem) NamespaceAliasDecl(0, SourceLocation(), SourceLocation(), 0,
                                      NestedNameSpecifierLoc(), 
                                      SourceLocation(), 0);
}

void UsingShadowDecl::anchor() { }

UsingShadowDecl *
UsingShadowDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(UsingShadowDecl));
  return new (Mem) UsingShadowDecl(0, SourceLocation(), 0, 0);
}

UsingDecl *UsingShadowDecl::getUsingDecl() const {
  const UsingShadowDecl *Shadow = this;
  while (const UsingShadowDecl *NextShadow =
         dyn_cast<UsingShadowDecl>(Shadow->UsingOrNextShadow))
    Shadow = NextShadow;
  return cast<UsingDecl>(Shadow->UsingOrNextShadow);
}

void UsingDecl::anchor() { }

void UsingDecl::addShadowDecl(UsingShadowDecl *S) {
  assert(std::find(shadow_begin(), shadow_end(), S) == shadow_end() &&
         "declaration already in set");
  assert(S->getUsingDecl() == this);

  if (FirstUsingShadow.getPointer())
    S->UsingOrNextShadow = FirstUsingShadow.getPointer();
  FirstUsingShadow.setPointer(S);
}

void UsingDecl::removeShadowDecl(UsingShadowDecl *S) {
  assert(std::find(shadow_begin(), shadow_end(), S) != shadow_end() &&
         "declaration not in set");
  assert(S->getUsingDecl() == this);

  // Remove S from the shadow decl chain. This is O(n) but hopefully rare.

  if (FirstUsingShadow.getPointer() == S) {
    FirstUsingShadow.setPointer(
      dyn_cast<UsingShadowDecl>(S->UsingOrNextShadow));
    S->UsingOrNextShadow = this;
    return;
  }

  UsingShadowDecl *Prev = FirstUsingShadow.getPointer();
  while (Prev->UsingOrNextShadow != S)
    Prev = cast<UsingShadowDecl>(Prev->UsingOrNextShadow);
  Prev->UsingOrNextShadow = S->UsingOrNextShadow;
  S->UsingOrNextShadow = this;
}

UsingDecl *UsingDecl::Create(ASTContext &C, DeclContext *DC, SourceLocation UL,
                             NestedNameSpecifierLoc QualifierLoc,
                             const DeclarationNameInfo &NameInfo,
                             bool IsTypeNameArg) {
  return new (C) UsingDecl(DC, UL, QualifierLoc, NameInfo, IsTypeNameArg);
}

UsingDecl *UsingDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(UsingDecl));
  return new (Mem) UsingDecl(0, SourceLocation(), NestedNameSpecifierLoc(),
                             DeclarationNameInfo(), false);
}

void UnresolvedUsingValueDecl::anchor() { }

UnresolvedUsingValueDecl *
UnresolvedUsingValueDecl::Create(ASTContext &C, DeclContext *DC,
                                 SourceLocation UsingLoc,
                                 NestedNameSpecifierLoc QualifierLoc,
                                 const DeclarationNameInfo &NameInfo) {
  return new (C) UnresolvedUsingValueDecl(DC, C.DependentTy, UsingLoc,
                                          QualifierLoc, NameInfo);
}

UnresolvedUsingValueDecl *
UnresolvedUsingValueDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(UnresolvedUsingValueDecl));
  return new (Mem) UnresolvedUsingValueDecl(0, QualType(), SourceLocation(),
                                            NestedNameSpecifierLoc(),
                                            DeclarationNameInfo());
}

void UnresolvedUsingTypenameDecl::anchor() { }

UnresolvedUsingTypenameDecl *
UnresolvedUsingTypenameDecl::Create(ASTContext &C, DeclContext *DC,
                                    SourceLocation UsingLoc,
                                    SourceLocation TypenameLoc,
                                    NestedNameSpecifierLoc QualifierLoc,
                                    SourceLocation TargetNameLoc,
                                    DeclarationName TargetName) {
  return new (C) UnresolvedUsingTypenameDecl(DC, UsingLoc, TypenameLoc,
                                             QualifierLoc, TargetNameLoc,
                                             TargetName.getAsIdentifierInfo());
}

UnresolvedUsingTypenameDecl *
UnresolvedUsingTypenameDecl::CreateDeserialized(ASTContext &C, unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, 
                                       sizeof(UnresolvedUsingTypenameDecl));
  return new (Mem) UnresolvedUsingTypenameDecl(0, SourceLocation(),
                                               SourceLocation(),
                                               NestedNameSpecifierLoc(),
                                               SourceLocation(),
                                               0);
}

void StaticAssertDecl::anchor() { }

StaticAssertDecl *StaticAssertDecl::Create(ASTContext &C, DeclContext *DC,
                                           SourceLocation StaticAssertLoc,
                                           Expr *AssertExpr,
                                           StringLiteral *Message,
                                           SourceLocation RParenLoc,
                                           bool Failed) {
  return new (C) StaticAssertDecl(DC, StaticAssertLoc, AssertExpr, Message,
                                  RParenLoc, Failed);
}

StaticAssertDecl *StaticAssertDecl::CreateDeserialized(ASTContext &C, 
                                                       unsigned ID) {
  void *Mem = AllocateDeserializedDecl(C, ID, sizeof(StaticAssertDecl));
  return new (Mem) StaticAssertDecl(0, SourceLocation(), 0, 0,
                                    SourceLocation(), false);
}

static const char *getAccessName(AccessSpecifier AS) {
  switch (AS) {
    case AS_none:
      llvm_unreachable("Invalid access specifier!");
    case AS_public:
      return "public";
    case AS_private:
      return "private";
    case AS_protected:
      return "protected";
  }
  llvm_unreachable("Invalid access specifier!");
}

const DiagnosticBuilder &clang::operator<<(const DiagnosticBuilder &DB,
                                           AccessSpecifier AS) {
  return DB << getAccessName(AS);
}

const PartialDiagnostic &clang::operator<<(const PartialDiagnostic &DB,
                                           AccessSpecifier AS) {
  return DB << getAccessName(AS);
}
