/* Copyright (C) 2013  Olga Yakovleva <yakovleva.o.v@gmail.com> */

/* This program is free software: you can redistribute it and/or modify */
/* it under the terms of the GNU Lesser General Public License as published by */
/* the Free Software Foundation, either version 3 of the License, or */
/* (at your option) any later version. */

/* This program is distributed in the hope that it will be useful, */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the */
/* GNU Lesser General Public License for more details. */

/* You should have received a copy of the GNU Lesser General Public License */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <stdint.h>
#include <stdexcept>
#include <memory>
#include <string>
#include <vector>
#include <iterator>
#include "utf8.h"
#include "core/engine.hpp"
#include "core/client.hpp"
#include "core/utf.hpp"
#include "core/document.hpp"
#include "native.h"

using namespace RHVoice;

namespace
{
  class java_exception: public std::runtime_error
  {
  public:
    java_exception():
      std::runtime_error("Java exception")
    {
    }
  };

  class internal_exception: public std::runtime_error
  {
  public:
    explicit internal_exception(const std::string& msg):
      std::runtime_error(msg)
    {
    }
  };

  class no_voices: public internal_exception
  {
  public:
    no_voices():
      internal_exception("No voices found")
    {
    }
  };

  class voice_not_found: public internal_exception
  {
  public:
    voice_not_found():
      internal_exception("Voice not found")
    {
    }
  };

  void inline check(JNIEnv* env)
  {
    if(env->ExceptionCheck())
      throw java_exception();
  }

  template<typename O>
  O inline check(JNIEnv* env,const O& obj)
  {
    if(env->ExceptionCheck())
      throw java_exception();
    return obj;
  }

  template<typename T>
  inline T* get_native_field(JNIEnv* env,jobject obj,jfieldID field_id)
  {
    long lval=env->GetLongField(obj,field_id);
    check(env);
    return reinterpret_cast<T*>(static_cast<intptr_t>(lval));
  }

  template<typename T>
  inline void set_native_field(JNIEnv* env,jobject obj,jfieldID field_id,T* ptr)
  {
    env->SetLongField(obj,field_id,reinterpret_cast<intptr_t>(ptr));
    check(env);
  }

  inline void clear_native_field(JNIEnv* env,jobject obj,jfieldID field_id)
  {
    void* ptr=0;
    env->SetLongField(obj,field_id,reinterpret_cast<intptr_t>(ptr));
    check(env);
  }

  inline jclass find_class(JNIEnv* env,const char* name)
  {
    jclass local_cls=check(env,env->FindClass(name));
    jclass global_cls=check(env,static_cast<jclass>(env->NewGlobalRef(local_cls)));
    return global_cls;
  }

  inline jmethodID get_method(JNIEnv* env,jclass cls,const char* name,const char* sig)
  {
    return check(env,env->GetMethodID(cls,name,sig));
  }

  inline jmethodID get_default_constructor(JNIEnv* env,jclass cls)
  {
    return get_method(env,cls,"<init>","()V");
  }

  inline jmethodID get_string_setter(JNIEnv* env,jclass cls,const char* name)
  {
    return get_method(env,cls,name,"(Ljava/lang/String;)V");
  }

  inline jmethodID get_string_getter(JNIEnv* env,jclass cls,const char* name)
  {
    return get_method(env,cls,name,"()Ljava/lang/String;");
  }

  inline jfieldID get_field(JNIEnv* env,jclass cls,const char* name,const char* sig)
  {
    return check(env,env->GetFieldID(cls,name,sig));
  }

  jstring string_to_jstring(JNIEnv* env,const std::string& cppstr)
  {
    std::vector<jchar> jchars;
    utf8::utf8to16(cppstr.begin(),cppstr.end(),std::back_inserter(jchars));
    return check(env,env->NewString(&jchars[0],jchars.size()));
  }

  class jstring_handle
  {
  public:
    jstring_handle(JNIEnv* env_,jstring jstr_):
    env(env_),
    jstr(jstr_),
      jlength(env->GetStringLength(jstr)),
      jchars(check(env,env->GetStringChars(jstr,0)))
    {
    }

    ~jstring_handle()
    {
      env->ReleaseStringChars(jstr,jchars);
    }

    const jchar* str() const
    {
      return jchars;
    }

    jsize length() const
    {
      return jlength;
    }

  private:
    JNIEnv* env;
    jstring jstr;
    jsize jlength;
    const jchar* jchars;

    jstring_handle(const jstring_handle&);
    jstring_handle& operator=(const jstring_handle&);
  };

  inline std::string jstring_to_string(JNIEnv* env,jstring jstr)
  {
    jstring_handle handle(env,jstr);
    std::string result;
    utf8::utf16to8(handle.str(),handle.str()+handle.length(),std::back_inserter(result));
    return result;
  }

  inline jobject new_object(JNIEnv* env,jclass cls,jmethodID default_constructor)
  {
    return check(env,env->NewObject(cls,default_constructor));
  }

  inline jobjectArray new_object_array(JNIEnv* env,jsize size,jclass cls,jmethodID default_constructor)
  {
    jobject default_value=new_object(env,cls,default_constructor);
    return check(env,env->NewObjectArray(size,cls,default_value));
  }

  inline void set_object_array_element(JNIEnv* env,jobjectArray array,jsize index,jobject value)
  {
    env->SetObjectArrayElement(array,index,value);
    check(env);
  }

  inline void call_string_setter(JNIEnv* env,jobject obj,jmethodID method_id,const std::string& str)
  {
    jstring jstr=string_to_jstring(env,str);
    env->CallVoidMethod(obj,method_id,jstr);
    check(env);
  }

  inline std::string call_string_getter(JNIEnv* env,jobject obj,jmethodID method)
  {
    jobject jstr=check(env,env->CallObjectMethod(obj,method));
    if(jstr==0)
      return std::string();
    return jstring_to_string(env,static_cast<jstring>(jstr));
  }

  jclass RHVoiceException_class;
  jfieldID data_field;
  jclass VoiceInfo_class;
  jmethodID VoiceInfo_constructor;
  jmethodID VoiceInfo_setName_method;
  jmethodID VoiceInfo_getName_method;
  jmethodID VoiceInfo_setLanguage_method;
  jclass LanguageInfo_class;
  jmethodID LanguageInfo_constructor;
  jmethodID LanguageInfo_setName_method;
  jmethodID LanguageInfo_setAlpha2Code_method;
  jmethodID LanguageInfo_setAlpha3Code_method;
  jmethodID LanguageInfo_setAlpha2CountryCode_method;
  jmethodID LanguageInfo_setAlpha3CountryCode_method;
  jclass SynthesisParameters_class;
  jmethodID SynthesisParameters_getMainVoice_method;
  jmethodID SynthesisParameters_getExtraVoice_method;
  jmethodID SynthesisParameters_getSSMLMode_method;
  jmethodID SynthesisParameters_getRate_method;
  jmethodID SynthesisParameters_getPitch_method;
  jmethodID SynthesisParameters_getVolume_method;

  class Data
  {
  public:
    Data()
    {
    }

    smart_ptr<engine> engine_ptr;

  private:
    Data(const Data&);
    Data& operator=(const Data&);
  };

  class speak_impl: public client
  {
  public:
    speak_impl(JNIEnv*,jobject,jstring,jobject,jobject);
    void operator()();

    bool play_speech(const short*,std::size_t);

  private:
    JNIEnv* env;
    Data* data;
    jstring input;
    jobject params;
    jobject client_object;
    jmethodID client_playSpeech_method;

    voice_list::const_iterator find_voice(jobject jvoice) const;

    speak_impl(const speak_impl&);
    speak_impl& operator=(const speak_impl&);
  };

  speak_impl::speak_impl(JNIEnv* env_,jobject self,jstring text,jobject synth_params,jobject tts_client):
    env(env_),
    data(get_native_field<Data>(env,self,data_field)),
    input(text),
    params(synth_params),
    client_object(tts_client)
  {
    jclass client_class=check(env,env->GetObjectClass(client_object));
    client_playSpeech_method=get_method(env,client_class,"playSpeech","([S)Z");
  }

  voice_list::const_iterator speak_impl::find_voice(jobject jvoice) const
  {
    voice_list::const_iterator result;
    if(jvoice!=0)
      {
        std::string name=call_string_getter(env,jvoice,VoiceInfo_getName_method);
        const voice_list& voices=data->engine_ptr->get_voices();
        voice_list::const_iterator found=voices.find(name);
        if(found==voices.end())
          throw voice_not_found();
        result=found;
      }
    return result;
  }

  void speak_impl::operator()()
  {
    document::init_params doc_params;
    doc_params.main_voice=find_voice(check(env,env->CallObjectMethod(params,SynthesisParameters_getMainVoice_method)));
    doc_params.extra_voice=find_voice(check(env,env->CallObjectMethod(params,SynthesisParameters_getExtraVoice_method)));
    std::string text=jstring_to_string(env,input);
    std::auto_ptr<document> doc;
    if(check(env,env->CallBooleanMethod(params,SynthesisParameters_getSSMLMode_method)))
      doc=document::create_from_ssml(data->engine_ptr,text.begin(),text.end(),doc_params);
    else
      doc=document::create_from_plain_text(data->engine_ptr,text.begin(),text.end(),content_text,doc_params);
    doc->speech_settings.relative.rate=check(env,env->CallDoubleMethod(params,SynthesisParameters_getRate_method));
    doc->speech_settings.relative.pitch=check(env,env->CallDoubleMethod(params,SynthesisParameters_getPitch_method));
    doc->speech_settings.relative.volume=check(env,env->CallDoubleMethod(params,SynthesisParameters_getVolume_method));
    doc->set_owner(*this);
    doc->synthesize();
  }
}

bool speak_impl::play_speech(const short* samples,std::size_t count)
{
  jshortArray jsamples=check(env,env->NewShortArray(count));
  env->SetShortArrayRegion(jsamples,0,count,samples);
  check(env);
  return check(env,env->CallBooleanMethod(client_object,client_playSpeech_method,jsamples));
}

#define TRY try {

#define CATCH1(env) } \
    catch(const java_exception&) {} \
    catch(const std::exception& e) {env->ThrowNew(RHVoiceException_class,e.what());} \
  return;

#define CATCH2(env,retval) } \
    catch(const java_exception&) {} \
    catch(const std::exception& e) {env->ThrowNew(RHVoiceException_class,e.what());} \
  return retval;

JNIEXPORT void JNICALL Java_com_github_olga_1yakovleva_rhvoice_TTSEngine_onClassInit
  (JNIEnv *env, jclass TTSEngine_class)
{
  TRY
    RHVoiceException_class=find_class(env,"com/github/olga_yakovleva/rhvoice/RHVoiceException");
  LanguageInfo_class=find_class(env,"com/github/olga_yakovleva/rhvoice/LanguageInfo");
  LanguageInfo_constructor=get_default_constructor(env,LanguageInfo_class);
  LanguageInfo_setName_method=get_string_setter(env,LanguageInfo_class,"setName");
  LanguageInfo_setAlpha2Code_method=get_string_setter(env,LanguageInfo_class,"setAlpha2Code");
  LanguageInfo_setAlpha3Code_method=get_string_setter(env,LanguageInfo_class,"setAlpha3Code");
  LanguageInfo_setAlpha2CountryCode_method=get_string_setter(env,LanguageInfo_class,"setAlpha2CountryCode");
  LanguageInfo_setAlpha3CountryCode_method=get_string_setter(env,LanguageInfo_class,"setAlpha3CountryCode");
  VoiceInfo_class=find_class(env,"com/github/olga_yakovleva/rhvoice/VoiceInfo");
  VoiceInfo_constructor=get_default_constructor(env,VoiceInfo_class);
  VoiceInfo_setName_method=get_string_setter(env,VoiceInfo_class,"setName");
  VoiceInfo_getName_method=get_string_getter(env,VoiceInfo_class,"getName");
  VoiceInfo_setLanguage_method=get_method(env,VoiceInfo_class,"setLanguage","(Lcom/github/olga_yakovleva/rhvoice/LanguageInfo;)V");
    SynthesisParameters_class=find_class(env,"com/github/olga_yakovleva/rhvoice/SynthesisParameters");
    SynthesisParameters_getMainVoice_method=get_method(env,SynthesisParameters_class,"getMainVoice","()Lcom/github/olga_yakovleva/rhvoice/VoiceInfo;");
    SynthesisParameters_getExtraVoice_method=get_method(env,SynthesisParameters_class,"getExtraVoice","()Lcom/github/olga_yakovleva/rhvoice/VoiceInfo;");
    SynthesisParameters_getSSMLMode_method=get_method(env,SynthesisParameters_class,"getSSMLMode","()Z");
    SynthesisParameters_getRate_method=get_method(env,SynthesisParameters_class,"getRate","()D");
    SynthesisParameters_getPitch_method=get_method(env,SynthesisParameters_class,"getPitch","()D");
    SynthesisParameters_getVolume_method=get_method(env,SynthesisParameters_class,"getVolume","()D");
  data_field=get_field(env,TTSEngine_class,"data","J");
  CATCH1(env)
}

JNIEXPORT void JNICALL Java_com_github_olga_1yakovleva_rhvoice_TTSEngine_onInit
(JNIEnv *env, jobject obj, jstring data_path, jstring config_path, jobjectArray resource_paths)
{
  TRY
  clear_native_field(env,obj,data_field);
  std::auto_ptr<Data> data(new Data);
  engine::init_params params;
  params.data_path=jstring_to_string(env,data_path);
  params.config_path=jstring_to_string(env,config_path);
  if(resource_paths!=0)
    {
      const jsize n=env->GetArrayLength(resource_paths);
      for(jsize i=0;i<n;++i)
        {
          jobject jstr=env->GetObjectArrayElement(resource_paths,i);
          check(env);
          params.resource_paths.push_back(jstring_to_string(env,static_cast<jstring>(jstr)));
        }
    }
  data->engine_ptr=engine::create(params);
  if(data->engine_ptr->get_voices().empty())
    throw no_voices();
  set_native_field(env,obj,data_field,data.get());
  data.release();
  CATCH1(env)
}

JNIEXPORT void JNICALL Java_com_github_olga_1yakovleva_rhvoice_TTSEngine_onShutdown
  (JNIEnv *env, jobject obj)
{
  TRY
  delete get_native_field<Data>(env,obj,data_field);
  clear_native_field(env,obj,data_field);
  CATCH1(env)
}

JNIEXPORT jobjectArray JNICALL Java_com_github_olga_1yakovleva_rhvoice_TTSEngine_doGetVoices
  (JNIEnv *env, jobject obj)
{
  TRY
    Data* data=get_native_field<Data>(env,obj,data_field);
  const voice_list& voices=data->engine_ptr->get_voices();
  std::size_t count=std::distance(voices.begin(),voices.end());
  jobjectArray result=new_object_array(env,count,VoiceInfo_class,VoiceInfo_constructor);
  std::size_t i=0;
  for(voice_list::const_iterator it=voices.begin();it!=voices.end();++it,++i)
    {
      jobject jvoice=new_object(env,VoiceInfo_class,VoiceInfo_constructor);
      const std::string& name=it->get_name();
      call_string_setter(env,jvoice,VoiceInfo_setName_method,name);
      const language_info& lang=*(it->get_language());
      jobject jlanguage=new_object(env,LanguageInfo_class,LanguageInfo_constructor);
      const std::string& language_name=lang.get_name();
      call_string_setter(env,jlanguage,LanguageInfo_setName_method,language_name);
      const std::string& alpha2_code=lang.get_alpha2_code();
      if(!alpha2_code.empty())
        call_string_setter(env,jlanguage,LanguageInfo_setAlpha2Code_method,alpha2_code);
      const std::string& alpha3_code=lang.get_alpha3_code();
      if(!alpha3_code.empty())
        call_string_setter(env,jlanguage,LanguageInfo_setAlpha3Code_method,alpha3_code);
      std::string country=it->get_country();
      if(country.length()>=3)
        {
          call_string_setter(env,jlanguage,LanguageInfo_setAlpha3CountryCode_method,country.substr(0,3));
          if(country.length()==6)
            call_string_setter(env,jlanguage,LanguageInfo_setAlpha2CountryCode_method,country.substr(4));
        }
      env->CallVoidMethod(jvoice,VoiceInfo_setLanguage_method,jlanguage);
      check(env);
      set_object_array_element(env,result,i,jvoice);
      env->DeleteLocalRef(jlanguage);
      env->DeleteLocalRef(jvoice);
    }
  return result;
  CATCH2(env,0)
}

JNIEXPORT void JNICALL Java_com_github_olga_1yakovleva_rhvoice_TTSEngine_doSpeak
  (JNIEnv *env, jobject self, jstring text, jobject synth_params, jobject tts_client)
{
  TRY
  speak_impl(env,self,text,synth_params,tts_client)();
  CATCH1(env);
}
